"""ui/tab_extract.py — Extract image tab (v2: spacious Build-tab style).

Extract an image to a folder. Format-routed (Stage 3a):
  .exfat            -> legacy OSFMount mount + copy (app._run_extract, unchanged)
  .ffpkg            -> UFS2Tool whole-image extract (in-tab worker)
  .ffpfs / .ffpfsc  -> mkpfs unpack (in-tab worker).
                       Stage 2: when the PFS backend is 'mkpfs' AND the bundled
                       MkPFS supports it, a .ffpfsc is unpacked with
                       `unpack --deep` so the game files inside the wrapped
                       exFAT come out DIRECTLY (native reader, no OSFMount). The
                       Lazy backend, and any deep-incompatible image or deep
                       failure, fall back to the existing nested-image flow
                       (a .ffpfsc unpacks to its NESTED IMAGE, extracted next
                       via the OSFMount .exfat route — unchanged).

Layout (single column, no embedded log — uses global OUTPUT LOG):

    ┌─ Page head ───────────────────────────────────────────────────────┐
    │ [📤]  Extract image                                               │
    │       Mount + copy contents from an .exfat image                  │
    ├───────────────────────────────────────────────────────────────────┤
    │ [i] Mounts via OSFMount, mirrors files preserving structure...    │
    ├───────────────────────────────────────────────────────────────────┤
    │ ┌─ Drop zone ─────────────────────────────────────────────────┐ │
    │ │   📤  Drop .exfat image here                                │ │
    │ │       or click to browse                                    │ │
    │ └─────────────────────────────────────────────────────────────┘ │
    │ ┌─ Detected strip (when file selected) ───────────────────────┐ │
    │ │ 🎮 Returnal · CUSA-XXXXX · v1.04 · 56.8 GB                   │ │
    │ └─────────────────────────────────────────────────────────────┘ │
    │ ┌─ Form fields ───────────────────────────────────────────────┐ │
    │ │ Source image                                                │ │
    │ │ [_______________________________________________] [Browse]  │ │
    │ │                                                             │ │
    │ │ Output directory                                            │ │
    │ │ [_______________________________________________] [Browse]  │ │
    │ │                                                             │ │
    │ │ Output folder name  • auto from filename                    │ │
    │ │ [_______________________________________________]           │ │
    │ │                                                             │ │
    │ │ ┌─ Progress card ────────────────────────────────────────┐ │ │
    │ │ │ status text         [████──────] ETA xx:xx              │ │ │
    │ │ └────────────────────────────────────────────────────────┘ │ │
    │ │                                                             │ │
    │ │ [📤 Extract Image]                                          │ │
    │ └─────────────────────────────────────────────────────────────┘ │
    └───────────────────────────────────────────────────────────────────┘

Output goes to the global OUTPUT LOG at the bottom (no embedded log
duplicating it).

Backwards compat: every `_extract_*` attribute the existing 5+ callbacks
read is preserved with the same name. Worker logic untouched.
"""

import os
import re
import time
import threading
import subprocess
import tkinter as tk
from tkinter import filedialog

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, _NO_WIN_FLAGS
from ui.shared.app_dialog import app_warning, app_error, app_confirm
from ui.shared.scroll import attach_scroll

from ui.shared.cards import DetectedGameStrip
from ui.shared.hero import GameHero
from ui.shared.forms import DropZone
from ui.shared.log_view import ConsoleView   # kept for compat; not packed
from ui.shared.page_head import (
    make_themed_button, info_banner, page_head, field_block)


def build_extract_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    # State
    app._extract_file_var   = tk.StringVar()
    app._extract_outdir_var = tk.StringVar()
    app._extract_name_var   = tk.StringVar(value='')
    app._extract_status_var = tk.StringVar(value='')
    app._extract_eta_var    = tk.StringVar(value='')
    app._extract_pct        = 0

    # ── Scrollable wrap ──
    canvas = tk.Canvas(parent, bg=COLORS['bg_1'], bd=0,
                       highlightthickness=0)
    canvas.pack(side='left', fill='both', expand=True)
    sb = tk.Scrollbar(parent, orient='vertical', command=canvas.yview,
                       bg=COLORS['bg_2'], troughcolor=COLORS['bg_1'])
    sb.pack(side='right', fill='y')
    canvas.configure(yscrollcommand=sb.set)

    inner = tk.Frame(canvas, bg=COLORS['bg_1'])
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')
    inner.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig(inner_id, width=e.width))
    attach_scroll(canvas)

    # ── Page head ──
    head = page_head(inner, '\U0001f4e4',
                     _('Extract image'),
                     _('Extract an .exfat, .ffpkg, or PFS image to a folder.'))
    head.pack(fill='x', padx=24, pady=(14, 12))

    # Force Dismount button on the right of the head
    def _force_dismount():
        try:
            app._force_dismount_all()
        except Exception:
            pass
    fd_btn = make_themed_button(head, '\u26a0  ' + _('Force Dismount'),
                                  command=_force_dismount, kind='ghost')
    _fd_shown = {'on': False}

    def _toggle_fd():
        if _fd_shown['on']:
            fd_btn.pack_forget()
            _fd_shown['on'] = False
            _adv_btn.config(text='\u2699  ' + _('Advanced'))
        else:
            fd_btn.pack(side='right', padx=(8, 0))
            _fd_shown['on'] = True
            _adv_btn.config(text='\u2699  ' + _('Advanced \u25be'))
    _adv_btn = make_themed_button(head, '\u2699  ' + _('Advanced'),
                                  command=_toggle_fd, kind='ghost')
    _adv_btn.pack(side='right', padx=(8, 0))

    # ── Info banner ──
    banner = info_banner(inner, _(
        'Unpacks a game image back into its files and folders. Needs '
        'roughly the image size in free space; progress appears in the '
        'OUTPUT LOG (click the bar at the bottom to expand). Note: a '
        '.ffpfsc unpacks to its nested image, not directly to game files. '
        '(Mechanics: .exfat via OSFMount, .ffpkg via UFS2Tool, '
        '.ffpfs/.ffpfsc via mkpfs.)'))
    banner.pack(fill='x', padx=24, pady=(0, 14))

    # ── Card ──
    card = tk.Frame(inner, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1)
    card.pack(fill='x', padx=24, pady=(0, 14))

    pad = tk.Frame(card, bg=COLORS['bg_2'])
    pad.pack(fill='x', padx=24, pady=18)

    # Numbered section headings (same look as the Convert tab)
    def _section(text, top=0):
        tk.Label(pad, text=text, font=(FONTS['h3'][0], 12, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['accent'], anchor='w').pack(
            fill='x', pady=(top, 6))

    _section(_('1.  Source image'))

    # ── Game hero (Build-style): cover + title + stats + status badge ──
    app._extract_hero = GameHero(
        pad,
        stats=[('Format', 'format'), ('Image Size', 'size'),
               ('Expected Files', 'files'), ('Output Space', 'output')],
        cover_glyph='\U0001f4e4', cover_size=150)
    app._extract_hero.pack(fill='x', pady=(0, 10))
    app._extract_hero.set_title(_('No image selected'), '')
    app._extract_hero.set_badge(_('SELECT AN IMAGE'), 'wait')
    # legacy attr kept for any external reference; no longer the focal UI
    app._extract_info_strip = None

    # Detected-format badge + plain-language note (filled by _refresh_detected)
    fmt_row = tk.Frame(pad, bg=COLORS['bg_2'])
    fmt_row.pack(fill='x', pady=(0, 12))
    app._extract_fmt_badge = tk.Label(
        fmt_row, text=_('NO IMAGE'),
        font=(FONTS['mono_sm'][0], 10, 'bold'),
        bg=COLORS['bg_4'], fg=COLORS['fg_4'], padx=10, pady=3)
    app._extract_fmt_badge.pack(side='left')
    app._extract_fmt_note = tk.Label(
        fmt_row, text=_('Select an image to detect its format.'),
        font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_4'],
        anchor='w', justify='left')
    app._extract_fmt_note.pack(side='left', padx=(10, 0), fill='x', expand=True)

    # Form fields
    field_block(pad, _('Source image'),
                 var=app._extract_file_var,
                 on_browse=lambda: _browse_any_image(app),
                 browse_text=_('Browse'),
                 hint=_('the image to extract (.exfat, .ffpkg, .ffpfs, .ffpfsc)'))

    _section(_('2.  Output folder'), top=4)

    field_block(pad, _('Output directory'),
                 var=app._extract_outdir_var,
                 on_browse=app._browse_extract_outdir,
                 browse_text=_('Browse'),
                 hint=_('where extracted files will land'))

    # Output folder name with auto-detected hint
    name_block = tk.Frame(pad, bg=COLORS['bg_2'])
    name_block.pack(fill='x', pady=(14, 0))
    name_lbl_row = tk.Frame(name_block, bg=COLORS['bg_2'])
    name_lbl_row.pack(fill='x')
    tk.Label(name_lbl_row, text=_('Output folder name'),
             font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w'
             ).pack(side='left')
    app._extract_auto_lbl = tk.Label(name_lbl_row,
        text='  \u2022  ' + _('auto from filename'),
        font=FONTS['meta'],
        bg=COLORS['bg_2'], fg=COLORS['success_hi'])
    app._extract_auto_lbl.pack(side='left')

    name_wrap = tk.Frame(name_block, bg=COLORS['field_bg'],
                          highlightbackground=COLORS['border_3'],
                          highlightthickness=1)
    name_wrap.pack(fill='x', pady=(6, 0))
    tk.Entry(name_wrap, textvariable=app._extract_name_var,
             font=FONTS['mono_sm'],
             bg=COLORS['field_bg'], fg=COLORS['field_fg'],
             insertbackground=COLORS['field_fg'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=8
             ).pack(fill='x')

    _section(_('3.  Extract'), top=4)

    # ── Progress card ──
    prog_card = tk.Frame(pad, bg=COLORS['bg_3'],
                          highlightbackground=COLORS['border_2'],
                          highlightthickness=1)
    prog_card.pack(fill='x', pady=(0, 14))
    pi = tk.Frame(prog_card, bg=COLORS['bg_3'])
    pi.pack(fill='x', padx=14, pady=12)

    status_row = tk.Frame(pi, bg=COLORS['bg_3'])
    status_row.pack(fill='x')
    tk.Label(status_row, textvariable=app._extract_status_var,
             font=FONTS['body_b'],
             bg=COLORS['bg_3'], fg=COLORS['fg_0'], anchor='w'
             ).pack(side='left', fill='x', expand=True)
    tk.Label(status_row, textvariable=app._extract_eta_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_4'], anchor='e'
             ).pack(side='right')

    # Slim progress bar
    bar_bg = tk.Frame(pi, bg=COLORS['bg_4'], height=8)
    bar_bg.pack(fill='x', pady=(8, 0))
    bar_bg.pack_propagate(False)
    app._extract_canvas = tk.Canvas(bar_bg, height=8, bg=COLORS['bg_4'],
                                     highlightthickness=0)
    app._extract_canvas.pack(fill='both', expand=True)
    app._extract_bar = app._extract_canvas.create_rectangle(
        0, 0, 0, 8, fill=COLORS['accent'], outline='')
    app._extract_canvas.bind('<Configure>',
        lambda e: app._update_extract_bar(app._extract_pct))

    # ── Demoted drop strip (supports the workflow, not the focal point) ──
    dz = DropZone(pad,
                  on_drop=lambda p: _on_drop(app, p),
                  on_click=lambda: _browse_any_image(app),
                  hint=None,
                  glyph='\U0001f4e4')
    dz.pack(fill='x', pady=(14, 0))
    try:
        # compress to a slim one-line strip
        _inner = dz._glyph_lbl.master
        _inner.pack_configure(padx=12, pady=7)
        dz._glyph_lbl.configure(font=(FONTS['body'][0], 12))
        dz._glyph_lbl.pack_configure(side='left', padx=(0, 8))
        dz._main_lbl.configure(
            text=_('Drop .exfat / .ffpkg / PFS image here \u00b7 or Browse above'),
            font=FONTS['mono_sm'])
        dz._main_lbl.pack_configure(side='left')
    except Exception:
        pass

    # ── Action button (dominant) ──
    app._extract_btn = make_themed_button(
        pad,
        text=_('Extract Image'),
        command=lambda: _route_extract(app),
        kind='success',
        icon='\U0001f4e4',
        font_size=13, padx=34, pady=14)
    app._extract_btn.pack(fill='x', pady=(18, 0))

    # ── Cancel (shown only while an in-tab worker runs) ──
    # Cooperative flag polled by the ffpkg mount+copy engine.
    app._extract_cancel = {'v': False}

    def _extract_request_cancel():
        app._extract_cancel['v'] = True
        try:
            app._extract_cancel_btn.config(state='disabled',
                                           text=_('Cancelling\u2026'))
        except Exception:
            pass

    app._extract_cancel_btn = make_themed_button(
        pad,
        text=_('Cancel Extract'),
        command=_extract_request_cancel,
        kind='ghost',
        font_size=11, padx=20, pady=8)
    # not packed here — the workers pack/forget it around a run

    # ── Hidden ConsoleView for back-compat ──
    # Legacy code paths (mostly in exfat_builder.py) still expect to
    # find `app._extract_log` as a Text widget. We create one but never
    # pack it — the actual output goes to the global OUTPUT LOG via the
    # `_extract_log_to_global` hook below.
    hidden_holder = tk.Frame(inner, bg=COLORS['bg_1'])
    # Don't pack; it stays as a no-op holder
    _cv = ConsoleView(hidden_holder, height=10)
    app._extract_log = _cv.text
    app._extract_log_console = _cv

    # Wire the hidden console: anything written to it also flows to
    # the global OUTPUT LOG so users see what's happening.
    _install_extract_log_mirror(app)

    # Trace the source-image var so the detected strip auto-populates
    app._extract_file_var.trace_add('write',
        lambda *a: _refresh_detected(app))
    app._extract_outdir_var.trace_add('write',
        lambda *a: _refresh_detected(app))


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _on_drop(app, path):
    """Drop handler — accept a single supported image file."""
    if path and _detect_format(path) and os.path.isfile(path):
        app._extract_file_var.set(path)
        base = os.path.splitext(os.path.basename(path))[0]
        app._extract_name_var.set(base)


def _refresh_detected(app):
    """Repaint the hero from the current source filename + output dir."""
    hero = getattr(app, '_extract_hero', None)
    if hero is None:
        return
    path = app._extract_file_var.get().strip()

    # output space (free on the chosen output dir) — independent of source
    def _free_bytes():
        od = app._extract_outdir_var.get().strip()
        try:
            if od and os.path.isdir(od):
                import shutil as _sh
                return _sh.disk_usage(od).free
        except Exception:
            pass
        return None

    def _free_str():
        fb = _free_bytes()
        return ('%.0f GB free' % (fb / 1024**3)) if fb is not None else '\u2014'

    def _space_verdict(img_size, fmt):
        fb = _free_bytes()
        if fb is None:
            return ('\u2014', 'wait')
        free_gb = fb / 1024**3
        if not img_size:
            return ('%.0f GB free' % free_gb, 'ok')
        req_gb = img_size / 1024**3
        # A compressed PFS needs the nested image AND its extraction, so
        # budget ~1.6x; everything else is roughly 1:1 plus slack.
        compressed = fmt == 'pfs_c'
        needed_gb = req_gb * (1.6 if compressed else 1.05)
        ok = free_gb >= needed_gb
        tail = ' (+ temp space)' if compressed else ''
        mark = '\u2713 Enough space' if ok else '\u2717 Low on space'
        txt = 'Need ~%.0f GB%s \u00b7 %.0f GB free \u00b7 %s' % (
            req_gb, tail, free_gb, mark)
        return (txt, 'ok' if ok else 'low')

    if not path or not os.path.isfile(path):
        hero.set_title(_('No image selected'), '')
        hero.reset_cover()
        for k in ('format', 'size', 'files'):
            hero.set_stat(k, '\u2014')
        hero.set_stat('output', _free_str())
        hero.set_badge(_('SELECT AN IMAGE'), 'wait')
        _badge = getattr(app, '_extract_fmt_badge', None)
        _note = getattr(app, '_extract_fmt_note', None)
        if _badge is not None:
            _badge.config(text=_('NO IMAGE'), bg=COLORS['bg_4'],
                          fg=COLORS['fg_4'])
        if _note is not None:
            _note.config(text=_('Pick an image above to extract \u2014 its '
                                'format is detected automatically.'),
                         fg=COLORS['fg_4'])
        return

    base = os.path.basename(path)
    name = base.rsplit('.', 1)[0]
    fmt = _detect_format(path)
    fmt_label = {'exfat': 'exFAT', 'ffpkg': 'ffpkg',
                 'pfs': 'PFS', 'pfs_c': 'PFS'}.get(fmt, '\u2014')

    # Format badge + note (what will come out, and any caveat)
    _badge = getattr(app, '_extract_fmt_badge', None)
    _note = getattr(app, '_extract_fmt_note', None)
    if _badge is not None or _note is not None:
        _btxt, _ntxt, _state = _format_meta(fmt)
        if _state == 'ok':
            _bg, _fg, _nfg = (COLORS['accent_08'], COLORS['accent_hi'],
                              COLORS['fg_4'])
        else:
            _bg, _fg, _nfg = (COLORS['danger_bg'], COLORS['danger_hi'],
                              COLORS['danger_hi'])
        if _badge is not None:
            _badge.config(text=_btxt, bg=_bg, fg=_fg)
        if _note is not None:
            _note.config(text=_ntxt, fg=_nfg)

    gid = None
    m = re.search(r'((?:CUSA|PPSA|PPLH)[-_ ]?\d{5})', name, re.IGNORECASE)
    if m:
        gid = m.group(1).upper().replace('_', '-').replace(' ', '-')
        if '-' not in gid:
            gid = gid[:4] + '-' + gid[4:]
    ver = None
    m = re.search(r'v(\d+\.\d+)', name, re.IGNORECASE)
    if m:
        ver = m.group(1)
    title = name
    if gid:
        title = re.sub(r'(?:CUSA|PPSA|PPLH)[-_ ]?\d{5}', '', title,
                       flags=re.IGNORECASE)
    if ver:
        title = re.sub(r'v\d+\.\d+', '', title, flags=re.IGNORECASE)
    title = re.sub(r'^[\s\-_.()]+|[\s\-_.()]+$', '', title).strip() or name

    try:
        size = os.path.getsize(path)
        size_str = ('%.2f GB' % (size / 1024**3) if size >= 1024**3
                    else '%d MB' % (size // 1024**2))
    except Exception:
        size_str = '\u2014'

    hero.set_title(title, (gid + (('  \u00b7  v' + ver) if ver else ''))
                   if gid else (('v' + ver) if ver else ''))
    hero.set_stat('format', fmt_label)
    hero.set_stat('size', size_str)
    hero.set_stat('files', '\u2014')   # known only after extract
    try:
        _img_sz = os.path.getsize(path)
    except Exception:
        _img_sz = 0
    _verdict_txt, _verdict_state = _space_verdict(_img_sz, fmt)
    hero.set_stat('output', _verdict_txt)
    hero.set_badge(_('READY TO EXTRACT'), 'ready')
    # cover: scope strictly to the SELECTED game — never scan the parent
    # directory (that returns a sibling game's icon0.png; see the cover-art
    # investigation). Try, in order: (1) a matching library entry's cover by
    # title ID, (2) the exact sibling dump folder for this image, (3) the
    # placeholder. _load_cover_art() is intentionally NOT changed.
    _dirn = os.path.dirname(path)
    _gid_norm = (gid or '').upper().replace('-', '')

    # (1) Library title-ID match (same approach the Library tab uses).
    _lib_cover = None
    if _gid_norm:
        for _g in getattr(app, '_lib_games', []) or []:
            if _g.get('title_id', '').upper().replace('-', '') == _gid_norm \
                    and _g.get('cover'):
                _lib_cover = _g['cover']
                break

    # (2) Exact sibling dump folder for this image: same basename as the
    # image, or a sibling folder whose name carries this title ID.
    _sib = None
    if not _lib_cover and _dirn and os.path.isdir(_dirn):
        _exact = os.path.join(_dirn, name)
        if os.path.isdir(_exact):
            _sib = _exact
        elif _gid_norm:
            try:
                for _d in os.listdir(_dirn):
                    _full = os.path.join(_dirn, _d)
                    if os.path.isdir(_full) and \
                            _gid_norm in _d.upper().replace('-', ''):
                        _sib = _full
                        break
            except Exception:
                _sib = None

    if _lib_cover:
        hero.set_cover_from_file(app, _lib_cover)
    elif _sib:
        hero.set_cover_from_folder(app, _sib)
    else:
        hero.reset_cover()


def _install_extract_log_mirror(app):
    """Hook the hidden _extract_log Text widget so any insert() call
    is mirrored to the global OUTPUT LOG. Idempotent per process."""
    txt = getattr(app, '_extract_log', None)
    if txt is None:
        return
    # per-widget guard: the Extract screen may be built more than once
    # during the workflow-nav transition; wrap each console exactly once.
    if getattr(txt, '_mirror_installed', False):
        return
    txt._mirror_installed = True

    original_insert = txt.insert

    def _wrapped_insert(*args, **kwargs):
        try:
            original_insert(*args, **kwargs)
        except Exception:
            pass
        # Mirror to global log. args is (index, chars, [tag]) typically.
        try:
            chars = args[1] if len(args) >= 2 else kwargs.get('chars', '')
            if chars and hasattr(app, '_log'):
                line = chars if chars.endswith('\n') else chars + '\n'
                if not line.startswith('[EXTRACT]') and \
                        not line.startswith('['):
                    line = '[EXTRACT] ' + line
                app._log(line)
                try:
                    app._toggle_log(force_open=True)
                except Exception:
                    pass
        except Exception:
            pass

    txt.insert = _wrapped_insert


# ─────────────────────────────────────────────────────────────────────────
# Stage 3a — format detection + routing (.exfat path untouched)
# ─────────────────────────────────────────────────────────────────────────
def _detect_format(path):
    """'exfat' | 'ffpkg' | 'pfs' (.ffpfs) | 'pfs_c' (.ffpfsc) | None.
    Extension-based, matching every other tab; the routed tool fails
    loudly on a mislabeled file."""
    p = (path or '').lower()
    if p.endswith('.exfat'):
        return 'exfat'
    if p.endswith('.ffpkg'):
        return 'ffpkg'
    if p.endswith('.ffpfsc'):
        return 'pfs_c'
    if p.endswith('.ffpfs'):
        return 'pfs'
    return None


# Badge text + plain-language note per detected format (what the user gets
# out, and the one caveat that matters).
_FORMAT_META = {
    'exfat': ('exFAT',
              'Standard exFAT image \u00b7 extracts to game files \u00b7 editable, can rebuild.'),
    'ffpkg': ('FFPKG',
              'Package image \u00b7 extracts to game files \u00b7 editable, can rebuild.'),
    'pfs':   ('FFPFS',
              'PFS image \u00b7 extracts to game files \u00b7 editable, can rebuild.'),
    'pfs_c': ('FFPFSC',
              'Compressed PFS \u00b7 extracts to a NESTED image (extract that next for game files) \u00b7 takes longer, needs temporary disk space.'),
}


def _format_meta(fmt):
    """(badge_text, note_text, state) for a detected format key.
    state is 'ok' for a recognised format, 'unknown' otherwise. No
    detection happens here \u2014 fmt comes straight from _detect_format."""
    if fmt in _FORMAT_META:
        b, n = _FORMAT_META[fmt]
        return (b, n, 'ok')
    return ('UNKNOWN', 'Unsupported image type.', 'unknown')


def _browse_any_image(app):
    """Tab-local browse for all supported formats. Mirrors the legacy
    _browse_extract_file behaviour (backslash munge + name autofill);
    the legacy method itself is untouched."""
    p = filedialog.askopenfilename(
        title=_('Select image to extract'),
        filetypes=[
            (_('Supported images'), '*.exfat *.ffpkg *.ffpfs *.ffpfsc'),
            ('exFAT images', '*.exfat'),
            ('ffpkg images', '*.ffpkg'),
            ('PFS images', '*.ffpfs *.ffpfsc'),
            (_('All files'), '*.*')])
    if not p:
        return
    p = p.replace('/', '\\')
    app._extract_file_var.set(p)
    base = os.path.splitext(os.path.basename(p))[0]
    app._extract_name_var.set(base)


def _route_extract(app):
    """Send the selected image to the right extractor by format."""
    path = app._extract_file_var.get().strip()
    fmt = _detect_format(path)
    if not path or not os.path.isfile(path) or fmt == 'exfat':
        # legacy OSFMount path, including its own missing-input messaging
        app._run_extract()
        return
    if fmt == 'ffpkg':
        _run_ffpkg_extract_tab(app)
    elif fmt in ('pfs', 'pfs_c'):
        _run_pfs_extract_tab(app, compressed=(fmt == 'pfs_c'))
    else:
        app_warning(app, _('Unsupported file type'),
            _('Supported image types: .exfat, .ffpkg, .ffpfs, .ffpfsc'))


def _alt_busy(app):
    if getattr(app, '_extract_alt_busy', False):
        return True
    try:
        if str(app._extract_btn.cget('state')) == 'disabled':
            return True
    except Exception:
        pass
    return False


def _set_xprog(app, pct, status=None, eta=None):
    """Feed the existing Extract progress card (thread-safe).

    pct=None means 'indeterminate' \u2014 update the status/eta text but leave
    the progress bar exactly where it is (used by the PFS proof-of-life
    elapsed clock, so the bar isn't repeatedly yanked back to 0)."""
    def _do():
        if pct is not None:
            try:
                app._extract_pct = pct
                app._update_extract_bar(pct)
            except Exception:
                pass
        try:
            if status is not None:
                app._extract_status_var.set(status)
            if eta is not None:
                app._extract_eta_var.set(eta)
        except Exception:
            pass
    app.after(0, _do)


def _xlog(app, line):
    try:
        app._log('[EXTRACT] %s\n' % line)
    except Exception:
        pass


def _count_files(folder):
    n = 0
    for _root, _dirs, files in os.walk(folder):
        n += len(files)
    return n


def _offer_open_folder(app, dest, title, message):
    """Show a completion dialog with an 'Open output folder?' affordance,
    matching the .exfat route's _extract_done. Opens dest on Yes."""
    try:
        if app_confirm(app, title, message + _('\\n\\nOpen output folder?')):
            try:
                os.startfile(dest)
            except Exception:
                pass
    except Exception:
        pass


def _fmt_xsize(b):
    if b >= 1024**3:
        return '%.1f GB' % (b / 1024**3)
    if b >= 1024**2:
        return '%.0f MB' % (b / 1024**2)
    return '%.0f KB' % (b / 1024)


def _validated_dest(app, kind_label):
    """Shared input validation for the in-tab workers. Returns
    (img, dest) or None. Mirrors the legacy validation flow."""
    img = app._extract_file_var.get().strip()
    outdir = app._extract_outdir_var.get().strip()
    name = app._extract_name_var.get().strip()
    if not img or not os.path.isfile(img):
        app_warning(app, _('Missing'),
            _('Please select a valid image file.'))
        return None
    if not outdir:
        app_warning(app, _('Missing'),
            _('Please select an output directory.'))
        return None
    if not name:
        name = os.path.splitext(os.path.basename(img))[0]
        app._extract_name_var.set(name)
    dest = os.path.normpath(os.path.join(outdir, name))
    if os.path.exists(dest):
        if not app_confirm(app, _('Folder exists'),
                dest + _('\n\nalready exists. Files will be '
                         'merged/overwritten. Continue?')):
            return None
    return img, dest


# ── .ffpkg — UFS2Tool whole-image extract ────────────────────────────────
def _spawn_ufs2_extract(app, exe, img, dest, on_line):
    """Run `UFS2Tool extract <img> <dest>`, streaming output lines to
    on_line. Returns the return code. Separated so tests can stub the
    subprocess."""
    proc = subprocess.Popen([exe, 'extract', img, dest],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            text=True, errors='replace',
                            creationflags=_NO_WIN_FLAGS)
    try:
        for line in proc.stdout:
            line = line.rstrip()
            if line:
                on_line(line)
    except Exception:
        pass
    return proc.wait()


def _run_ffpkg_extract_tab(app):
    if _alt_busy(app):
        return
    v = _validated_dest(app, 'ffpkg')
    if not v:
        return
    img, dest = v
    app._extract_alt_busy = True
    try:
        app._extract_btn.config(state='disabled', text=_('Extracting...'))
    except Exception:
        pass
    app._extract_cancel['v'] = False
    try:
        app._extract_cancel_btn.config(state='normal',
                                       text=_('Cancel Extract'))
        app._extract_cancel_btn.pack(fill='x', pady=(8, 0))
    except Exception:
        pass
    _set_xprog(app, 0, _('Preparing\u2026'), '')

    def _worker():
        # v3.6.2: mount + robocopy via the shared engine on the app — no
        # `UFS2Tool extract`, which reads each file fully into RAM and
        # fails on large game files. The engine always unmounts.
        def _log(text):
            _xlog(app, text.rstrip('\n'))

        def _progress(pct, status, eta):
            _set_xprog(app, pct, status, eta)

        try:
            summary = app._ffpkg_mount_copy_extract(
                img, dest, _log, _progress,
                cancel_check=lambda: app._extract_cancel['v'])
            ok = summary.get('ok', False)
            if ok:
                n  = summary.get('files', 0)
                sz = summary.get('bytes', 0)
                _set_xprog(app, 100,
                    _('Extracted %d files \u00b7 %s') % (n, _fmt_xsize(sz)),
                    _('done'))
                app.after(0, lambda d=dest, _n=n, _s=sz: _offer_open_folder(
                    app, d, _('Extraction complete'),
                    _('Extracted to:\n%s\n\n%d files \u00b7 %s')
                    % (d, _n, _fmt_xsize(_s))))
            elif summary.get('cancelled'):
                # User pressed Cancel Extract: the engine stopped the copy
                # and unmounted. Partial output stays on disk; nothing to
                # pop up — the status line says it all.
                _set_xprog(app, 0, _('Cancelled'), '')
            elif summary.get('oversize_files'):
                # >= 4 GB file(s): the bundled UFS2Tool Dokan mount cannot
                # serve them. Honest limitation message (not a generic rc=9).
                _ov = summary['oversize_files']
                _lines = '\n'.join(
                    '- %s \u2014 %.1f GB' % (nm, sz / 1024**3)
                    for nm, sz in _ov)
                _msg = (
                    'This .ffpkg contains files over 4 GB that the current '
                    'bundled UFS2Tool Dokan mount cannot extract.\n\n'
                    'Affected files:\n' + _lines + '\n\n'
                    'This is a limitation of the current UFS2Tool mount '
                    'backend, not a problem with your image. A fixed UFS2Tool '
                    'build or alternative extraction backend is required.')
                _set_xprog(app, 0, _('Cannot extract \u2014 files over 4 GB'),
                           '')
                app.after(0, lambda m=_msg: app_error(app, 
                    _('Cannot extract \u2014 files over 4 GB'), _(m)))
            elif summary.get('dokan_missing'):
                _set_xprog(app, 0, _('Dokan driver required'), '')
                def _ask():
                    if app_confirm(app, _('Dokan required'),
                            _('Extracting a .ffpkg to a folder mounts it as '
                              'a drive, which needs the free Dokan driver.'
                              '\n\nOpen the download page now?')):
                        import webbrowser
                        webbrowser.open(
                            'https://github.com/dokan-dev/dokany/'
                            'releases/latest')
                app.after(0, _ask)
            else:
                err = summary.get('error') or ('rc=%s' % summary.get('rc'))
                _set_xprog(app, 0, _('Extract failed'), '')
                app.after(0, lambda: app_error(app, 
                    _('Extract failed'),
                    _('Extraction failed:\n\n') + err +
                    _('\n\nSee the OUTPUT LOG for details.')))
        except Exception as e:
            _xlog(app, 'ffpkg extract error: %r' % e)
            _set_xprog(app, 0, _('Extract failed'), '')
        finally:
            def _done():
                app._extract_alt_busy = False
                try:
                    app._extract_btn.config(state='normal',
                                            text=_('Extract Image'))
                except Exception:
                    pass
                try:
                    app._extract_cancel_btn.pack_forget()
                except Exception:
                    pass
            app.after(0, _done)
    threading.Thread(target=_worker, daemon=True).start()


# ── .ffpfs / .ffpfsc — mkpfs unpack ──────────────────────────────────────
def _run_pfs_extract_tab(app, compressed):
    if _alt_busy(app):
        return
    v = _validated_dest(app, 'pfs')
    if not v:
        return
    img, dest = v
    overwrite = os.path.exists(dest)
    app._extract_alt_busy = True
    try:
        app._extract_btn.config(state='disabled', text=_('Extracting...'))
    except Exception:
        pass
    _set_xprog(app, 0, _('Starting PFS unpack\u2026'), '')

    def _worker():
        disk_full = {'hit': False}
        # v3.6.4 (issue #40): destination-size poller state. Defined up
        # here so the finally below can stop it on EVERY exit path,
        # including the early mkpfs-not-available return.
        run = {'on': False, 'last_tick': 0.0}

        def _log_dl(l):
            _xlog(app, l)
            low = str(l).lower()
            if 'no space left' in low or 'errno 28' in low:
                disk_full['hit'] = True

        try:
            from ui.mkpfs_runner import (run_mkpfs, mkpfs_version,
                                         mkpfs_supports_deep_unpack)
            if not mkpfs_version():
                _set_xprog(app, 0, _('mkpfs not available'), '')
                app.after(0, lambda: app_error(app,
                    _('mkpfs not available'),
                    _('mkpfs is not bundled in this build.')))
                return

            # Stage 2 — backend + capability gate for `unpack --deep`.
            # The Settings tab stores the PFS convert backend ('lazy' is
            # the default; 'mkpfs' = PSBrew MkPFS). Deep unpack is only
            # attempted when ALL of these hold:
            #   * the image is a .ffpfsc (a wrapped exFAT inside PFS),
            #   * the user picked the 'mkpfs' backend,
            #   * the bundled MkPFS build actually supports --deep.
            # Anything else keeps the existing behaviour byte-for-byte:
            # plain `unpack` → nested image → OSFMount .exfat route.
            # The gate is logged explicitly so a support log shows WHY
            # OSFMount was or wasn't used for a given run.
            #
            # Settings access is defensive: the tab can be built before
            # _settings exists (tests), so any failure means 'lazy'.
            try:
                _backend = (str(app._settings.get('pfs_backend', 'lazy'))
                            .strip().lower() or 'lazy')
            except Exception:
                _backend = 'lazy'
            if _backend not in ('lazy', 'mkpfs'):
                _backend = 'lazy'
            _deep_capable = False
            try:
                _deep_capable = bool(mkpfs_supports_deep_unpack())
            except Exception:
                _deep_capable = False
            _mkpfs_ver = mkpfs_version() or '(unknown)'
            use_deep = bool(compressed and _backend == 'mkpfs'
                            and _deep_capable)

            # Always log the decision (backend, version, capability and
            # whether OSFMount is in play) before anything runs.
            _xlog(app, '[EXTRACT] PFS backend selected: %s' % _backend)
            _xlog(app, '[EXTRACT] MkPFS version: %s' % _mkpfs_ver)
            _xlog(app, '[EXTRACT] Deep unpack supported: %s'
                  % ('yes' if _deep_capable else 'no'))
            if use_deep:
                _xlog(app, '[EXTRACT] OSFMount: SKIPPED for this run — '
                           'using mkpfs unpack --deep (native, mount-free).')
            elif compressed and _backend == 'mkpfs' and not _deep_capable:
                _xlog(app, '[EXTRACT] OSFMount: deep unpack unavailable in '
                           'this MkPFS build — using existing nested-image '
                           '+ OSFMount path.')
            elif compressed and _backend == 'lazy':
                _xlog(app, '[EXTRACT] OSFMount: Lazy backend selected — '
                           'using existing nested-image + OSFMount path '
                           '(unchanged).')

            argv = ['unpack', img, dest]
            if overwrite:
                argv.append('--overwrite')
            if use_deep:
                argv.append('--deep')
            _xlog(app, 'mkpfs ' + ' '.join(argv))

            # Stage 2 pre-scan (deep only): `mkpfs tree --deep` walks the
            # wrapped exFAT natively and prints one line per entry. It
            # costs a few seconds on a big image but gives an honest entry
            # count up front (and proves the native reader can open the
            # image before the long unpack starts). Any failure here is
            # logged and ignored — the unpack itself decides success.
            if use_deep:
                try:
                    _scan_argv = ['tree', '--deep', img]
                    _xlog(app, '[EXTRACT] Pre-scan (native, no OSFMount): mkpfs '
                          + ' '.join(_scan_argv))
                    _scan = {'entries': 0}

                    def _scan_log(line):
                        s = str(line).rstrip()
                        # tree output: one entry per line, drawn with the
                        # usual '|--' / '`--' branch prefixes. Count those
                        # and ignore headers/blank lines. (The lines are
                        # NOT echoed to the OUTPUT LOG — far too noisy.)
                        if s and ('|--' in s or '`--' in s):
                            _scan['entries'] += 1

                    from ui.mkpfs_runner import run_mkpfs as _rm_scan
                    _src = _rm_scan(_scan_argv, log_cb=_scan_log)
                    if _src == 0 and _scan['entries'] > 0:
                        _xlog(app, '[EXTRACT] Pre-scan: ~%d entries inside '
                                   'the image (native tree, files + dirs).'
                              % _scan['entries'])
                    else:
                        _xlog(app, '[EXTRACT] Pre-scan returned no usable '
                                   'count (rc=%s) — proceeding; progress falls '
                                   'back to the destination-size poller.'
                              % _src)
                except Exception as _se:
                    _xlog(app, '[EXTRACT] Pre-scan skipped (%r) — proceeding '
                               'with destination-size poller.'
                          % _se)

            # v3.6.4 (issue #40): mkpfs's CLI runs `unpack` with
            # progress=None upstream (cli.py, cli_mkpfs_extract_run), so
            # unlike `pack` there are NO progress lines to parse and the
            # bar sat at "Starting PFS unpack…" until completion. Drive it
            # with a destination-size poller instead — the same approach
            # as the .exfat and .ffpkg extract routes. The mkpfs progress
            # callback stays wired and takes precedence if a future mkpfs
            # build emits unpack progress: the poller only speaks after 3s
            # of parser silence (mirrors the pipeline's pack poller).
            #
            # Percent reference is the IMAGE size. For an uncompressed
            # .ffpfs the extracted payload is slightly under the image
            # size, so percent is accurate. For a .ffpfsc the decompressed
            # output can EXCEED the image size (ratio unknown in advance),
            # so percent is capped at 99 until mkpfs returns — the bytes /
            # speed line stays truthful throughout, and the success path
            # below is the only thing that sets 100.
            try:
                total_ref = os.path.getsize(img)
            except Exception:
                total_ref = 0
            import time as _t
            t0 = _t.monotonic()
            run['on'] = True

            def _on_progress(phase, pct, total, detail):
                # mkpfs spoke: it owns the bar for the next 3 s (the
                # poller checks last_tick before it writes anything).
                run['last_tick'] = _t.monotonic()
                _set_xprog(app, pct,
                    _('Unpacking PFS · %s') % (detail or phase),
                    '%d%%' % int(pct))

            # Proof-of-life clock. A deep unpack of a 50+ GB image can sit
            # for minutes before the first byte lands in `dest` (the
            # native reader walks the whole exFAT first), and even the
            # plain unpack has long silent stretches. The poller therefore
            # ALWAYS repaints the status line every second with the
            # elapsed time, and only adds bytes / rate / ETA once output
            # actually starts growing. The bar itself is left alone until
            # there is real progress to show (pct=None → indeterminate),
            # so it is never yanked back to 0 by an empty poll.
            def _elapsed_str(secs):
                secs = int(max(0, secs))
                h, rem = divmod(secs, 3600)
                m, s = divmod(rem, 60)
                return '%02d:%02d:%02d' % (h, m, s)

            def _poll():
                while run['on']:
                    try:
                        # Only speak when mkpfs has been quiet for 3 s —
                        # its own progress callback takes precedence.
                        if _t.monotonic() - run['last_tick'] >= 3.0:
                            el = max(0.001, _t.monotonic() - t0)
                            el_str = _elapsed_str(el)
                            cur = 0
                            try:
                                cur = app._get_folder_size(dest)
                            except Exception:
                                cur = 0
                            if not run['on']:
                                return
                            if cur > 0:
                                # Output is growing: bytes, files, rate,
                                # capped percent and a rough ETA.
                                rate = cur / el
                                try:
                                    nfiles = _count_files(dest)
                                except Exception:
                                    nfiles = 0
                                pct = (min(99.0, cur / total_ref * 100.0)
                                       if total_ref else 0.0)
                                eta = ''
                                if rate > 0 and total_ref and cur < total_ref:
                                    left = (total_ref - cur) / rate
                                    eta = '~%dm %02ds left (est.)' % (
                                        int(left) // 60, int(left) % 60)
                                status = _('Unpacking PFS · %d files · '
                                           '%s written · %.0f MB/s · '
                                           'Elapsed %s') % (
                                    nfiles, _fmt_xsize(cur),
                                    rate / 1048576, el_str)
                                _set_xprog(app, pct, status, eta)
                            else:
                                # Nothing on disk yet: just prove we are
                                # alive. pct=None leaves the bar where it
                                # is instead of resetting it.
                                _set_xprog(app, None,
                                    _('Unpacking PFS · Elapsed %s · working…')
                                    % el_str,
                                    _('working…'))
                    except Exception:
                        pass
                    _t.sleep(1)

            threading.Thread(target=_poll, daemon=True).start()

            rc = run_mkpfs(argv, log_cb=_log_dl, progress_cb=_on_progress)
            run['on'] = False   # poller off before the final 100% set

            # Stage 2 fallback: a deep unpack that fails for any reason
            # other than a full disk is retried as the plain unpack, so the
            # user ends up on the proven nested-image + OSFMount path
            # instead of a bare error. Disk-full is NOT retried (the plain
            # unpack would just fail the same way).
            if use_deep and rc != 0 and not disk_full['hit']:
                _xlog(app, '[EXTRACT] unpack --deep failed (rc=%s) — falling '
                           'back to plain unpack (nested image + OSFMount path).'
                      % rc)
                use_deep = False
                plain_argv = ['unpack', img, dest]
                if overwrite:
                    plain_argv.append('--overwrite')
                _xlog(app, 'mkpfs ' + ' '.join(plain_argv))
                run['on'] = True
                run['last_tick'] = _t.monotonic()
                threading.Thread(target=_poll, daemon=True).start()
                rc = run_mkpfs(plain_argv, log_cb=_log_dl,
                               progress_cb=_on_progress)
                run['on'] = False

            if rc == 0 and os.path.isdir(dest):
                nested = []
                if compressed:
                    try:
                        for f in sorted(os.listdir(dest)):
                            fp = os.path.join(dest, f)
                            if os.path.isfile(fp) and (
                                    f.lower().endswith(('.exfat', '.ffpkg'))
                                    or f.lower() == 'pfs_image.dat'):
                                nested.append(f)
                    except Exception:
                        pass
                if nested:
                    # A nested image came out. With --deep that means the
                    # image was not deep-compatible (MkPFS fell back to the
                    # plain unpack itself) — say so, then continue exactly
                    # as before.
                    if use_deep:
                        _xlog(app, '[EXTRACT] unpack --deep produced a nested '
                                   'image (image not deep-compatible) — '
                                   'falling back to nested-image + OSFMount '
                                   'flow.')
                    nname = nested[0]
                    try:
                        nsz = _fmt_xsize(
                            os.path.getsize(os.path.join(dest, nname)))
                    except Exception:
                        nsz = ''
                    _set_xprog(app, 100,
                        _('Unpacked nested image · %s') % nname,
                        _('done'))
                    app.after(0, lambda d=dest, _nn=nname, _nz=nsz:
                        _offer_open_folder(app, d,
                            _('Unpacked to nested image'),
                            _('This .ffpfsc unpacked to its NESTED IMAGE, '
                              'not game files:\n\n  %s  %s\n  in %s\n\n'
                              'To reach the game files, extract that image '
                              'next — select it as the source above. '
                              '(Automatic chaining is not enabled yet.)')
                            % (_nn, ('(%s)' % _nz) if _nz else '', d)))
                else:
                    if use_deep:
                        _xlog(app, '[EXTRACT] unpack --deep extracted game '
                                   'files directly — OSFMount was not used.')
                    n = _count_files(dest)
                    sz = app._get_folder_size(dest)
                    _set_xprog(app, 100,
                        _('Extracted %d files · %s')
                        % (n, _fmt_xsize(sz)), _('done'))
                    app.after(0, lambda d=dest, _n=n, _s=sz: _offer_open_folder(
                        app, d, _('Extraction complete'),
                        _('Extracted to:\n%s\n\n%d files · %s\n\n'
                          'Edit the files, then rebuild from the Build '
                          'tab.') % (d, _n, _fmt_xsize(_s))))
            elif disk_full['hit']:
                _set_xprog(app, 0, _('Disk full during unpack'), '')
                app.after(0, lambda: app_error(app,
                    _('Disk full'),
                    _('The destination drive ran out of space during '
                      'the unpack.')))
            else:
                _set_xprog(app, 0,
                    _('mkpfs unpack failed (rc=%s)') % rc, '')
                app.after(0, lambda: app_error(app,
                    _('Unpack failed'),
                    _('mkpfs unpack failed (rc=%s).\n\nSee the OUTPUT '
                      'LOG for details.') % rc))
        except Exception as e:
            _xlog(app, 'PFS extract error: %r' % e)
            _set_xprog(app, 0, _('Extract failed'), '')
        finally:
            run['on'] = False   # stop the size poller on any exit path
            def _done():
                app._extract_alt_busy = False
                try:
                    app._extract_btn.config(state='normal',
                                            text=_('Extract Image'))
                except Exception:
                    pass
            app.after(0, _done)
    threading.Thread(target=_worker, daemon=True).start()
