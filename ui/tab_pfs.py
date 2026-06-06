"""ui/tab_pfs.py — Build PFS Image tab (.ffpfsc for ShadowMount+ / MicroMount)

All routes use the official ShadowMount+ method: the .ffpfsc is a
compressed PFS *container* holding one nested image, never raw game
files (compressed PFS zero-pads to 64 KB sectors, so it is only valid
as an outer wrapper).

  BUILD    dump folder → uncompressed nested PFS (pfs_image.dat)
           → packed into .ffpfsc           (two-step, per spec)
  CONVERT  .exfat / .ffpkg → packed directly into .ffpfsc as the
           nested image (single 'mkpfs pack file' — the official
           MicroMount/MkPFS method; the nested name keeps its
           .exfat / .ffpkg extension)
  EXTRACT  .ffpfs / .ffpfsc → folder

The nested PFS filename must be exactly 'pfs_image.dat' (no leading
dot) — nested images are recognised by that literal name or by the
.ffpfs / .exfat / .ffpkg extension.
"""

import os
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from tkinter_theme import COLORS, FONTS
from ui.shared.page_head import (
    make_themed_button, info_banner, page_head, field_block)

def _fmt_size(b):
    if b >= 1024**3:
        return '%.2f GB' % (b / 1024**3)
    if b >= 1024**2:
        return '%.0f MB' % (b / 1024**2)
    return '%.0f KB' % (b / 1024)


def _free_space(path):
    """Return free bytes on the drive containing `path`, or None."""
    try:
        import shutil as _sh
        # Use the directory (path may not exist yet)
        d = path if os.path.isdir(path) else os.path.dirname(path) or path
        return _sh.disk_usage(d).free
    except Exception:
        return None


def _check_space_or_warn(out_dir, needed_bytes, parent):
    """Warn if the output drive may not have room. Returns True to proceed."""
    free = _free_space(out_dir)
    if free is None:
        return True  # can't tell — let it proceed
    # PFS output is usually similar size or smaller, but leave headroom.
    # Warn if free space is less than ~95% of the source (worst case: no compression).
    if free < needed_bytes * 0.95:
        from tkinter import messagebox
        return messagebox.askyesno(
            'Low disk space',
            'The output drive has %s free, but the source is %s.\n\n'
            'PFS is usually similar size or smaller, but if this game '
            'compresses poorly you may run out of space mid-build.\n\n'
            'Continue anyway?'
            % (_fmt_size(free), _fmt_size(needed_bytes)))
    return True


def _get_folder_size(folder):
    total = 0
    try:
        stack = [folder]
        while stack:
            cur = stack.pop()
            try:
                with os.scandir(cur) as it:
                    for e in it:
                        try:
                            if e.is_file(follow_symlinks=False):
                                total += e.stat(follow_symlinks=False).st_size
                            elif e.is_dir(follow_symlinks=False):
                                stack.append(e.path)
                        except Exception:
                            pass
            except Exception:
                pass
    except Exception:
        pass
    return total


def _count_files(folder):
    n = 0
    try:
        stack = [folder]
        while stack:
            cur = stack.pop()
            try:
                with os.scandir(cur) as it:
                    for e in it:
                        try:
                            if e.is_file(follow_symlinks=False):
                                n += 1
                            elif e.is_dir(follow_symlinks=False):
                                stack.append(e.path)
                        except Exception:
                            pass
            except Exception:
                pass
    except Exception:
        pass
    return n



# ── Stage weights ────────────────────────────────────────────────────
_STAGES_BUILD = [
    ('pack',     'Pack PFS',    0,  50),
    ('compress', 'Compress',   50, 100),
]
_STAGES_EXTRACT = [('extract', 'Extract', 0, 100)]


def _find_game_root(folder):
    """Return the path whose direct child is sce_sys/param.json.

    Checks the folder itself, then one level of subfolders. Returns None
    if no sce_sys/param.json is found at root or one level down.
    """
    pj = os.path.join(folder, 'sce_sys', 'param.json')
    if os.path.isfile(pj):
        return folder
    # One level down (handles dumps wrapped in an extra folder)
    try:
        for entry in os.scandir(folder):
            if entry.is_dir(follow_symlinks=False):
                sub_pj = os.path.join(entry.path, 'sce_sys', 'param.json')
                if os.path.isfile(sub_pj):
                    return entry.path
    except Exception:
        pass
    return None


def _seed_empty_dir_markers(root):
    """mkpfs drops empty directories (it packs files only and rebuilds the
    tree from file paths). PS5 games \u2014 especially Unreal Engine titles \u2014
    rely on empty directories existing for runtime path resolution
    (e.g. '../../../game/game.uproject').  Seed a zero-byte '.pfskeep' marker
    into every empty directory so it survives the PFS round-trip.

    Returns the list of created marker paths so they can be removed after.
    """
    created = []
    try:
        for dirpath, dirnames, filenames in os.walk(root):
            if not filenames and not dirnames:
                marker = os.path.join(dirpath, '.pfskeep')
                try:
                    with open(marker, 'wb'):
                        pass
                    created.append(marker)
                except Exception:
                    pass
    except Exception:
        pass
    return created


def _remove_markers(markers):
    for m in markers or ():
        try:
            if os.path.isfile(m):
                os.remove(m)
        except Exception:
            pass


def _live_file_size(target):
    """Return the current on-disk size of `target`, checking the '.tmp'
    file mkpfs writes to first, then the final path. Returns bytes or None."""
    for p in (target + '.tmp', target):
        try:
            if os.path.isfile(p):
                return os.path.getsize(p)
        except Exception:
            pass
    return None


def _supports_hardlinks(directory):
    """Test whether `directory` supports hard links (NTFS does; exFAT/FAT32
    do not). mkpfs 'pack file' stages its source via os.link, which fails on
    exFAT drives, so we use this to decide where staging can happen."""
    try:
        import tempfile as _tf
        with _tf.TemporaryDirectory(dir=directory) as d:
            a = os.path.join(d, 'a')
            b = os.path.join(d, 'b')
            with open(a, 'wb'):
                pass
            try:
                os.link(a, b)
                return True
            except OSError:
                return False
    except Exception:
        return False


def build_pfs_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    canvas = tk.Canvas(parent, bg=COLORS['bg_1'], bd=0, highlightthickness=0)
    canvas.pack(side='left', fill='both', expand=True)
    sb = ttk.Scrollbar(parent, orient='vertical', command=canvas.yview)
    sb.pack(side='right', fill='y')
    canvas.configure(yscrollcommand=sb.set)
    inner = tk.Frame(canvas, bg=COLORS['bg_1'])
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')
    inner.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig(inner_id, width=e.width))
    canvas.bind('<MouseWheel>', lambda e:
        canvas.yview_scroll(int(-e.delta / 120), 'units'))

    # ── Page head ────────────────────────────────────────────────────
    head = page_head(inner, '\U0001f5dc',
                     'Build PFS Image',
                     'Pack a game dump into a .ffpfs / .ffpfsc image for ShadowMount+ / MicroMount.')
    head.pack(fill='x', padx=24, pady=(14, 8))

    info_banner(inner,
        '\u2139  Choose the output format below.  .ffpfsc is a compressed '
        'container (smaller on disk, but reads are capped ~150\u2013250 MB/s on '
        'console).  .ffpfs is an uncompressed PFS (larger, but full read speed '
        'and a single fast pass \u2014 no temporary file).  Large games are '
        'I/O-heavy, so allow time and free space.'
    ).pack(fill='x', padx=24, pady=(0, 12))

    badge_var = tk.StringVar(value='Checking mkpfs\u2026')
    badge_row = tk.Frame(inner, bg=COLORS['bg_1'])
    badge_row.pack(fill='x', padx=24, pady=(0, 14))
    badge_lbl = tk.Label(badge_row, textvariable=badge_var,
                          font=FONTS['mono_sm'],
                          bg=COLORS['bg_1'], fg=COLORS['fg_4'], anchor='w')
    badge_lbl.pack(side='left')

    def _check_badge():
        from ui.mkpfs_runner import mkpfs_version
        ver = mkpfs_version()
        if ver:
            parent.after(0, lambda: (badge_var.set('\u2713  mkpfs  ' + ver),
                                      badge_lbl.config(fg=COLORS['teal'])))
        else:
            parent.after(0, lambda: (badge_var.set('\u2717  mkpfs not available'),
                                      badge_lbl.config(fg=COLORS['danger'])))
    threading.Thread(target=_check_badge, daemon=True).start()

    # ── Path selector ────────────────────────────────────────────────
    path_var = tk.StringVar(value='convert')  # 'convert' | 'extract'

    sel_frame = tk.Frame(inner, bg=COLORS['bg_1'])
    sel_frame.pack(fill='x', padx=24, pady=(0, 14))

    def _make_path_tile(parent_f, key, icon, title, subtitle):
        outer = tk.Frame(parent_f, bg=COLORS['bg_2'],
                         highlightthickness=2,
                         highlightbackground=COLORS['border_2'],
                         cursor='hand2')
        outer.pack(side='left', fill='both', expand=True, padx=(0, 8))
        icon_lbl = tk.Label(outer, text=icon, font=(FONTS['h2'][0], 20),
                            bg=COLORS['bg_2'], fg=COLORS['fg_4'])
        icon_lbl.pack(pady=(18, 4))
        title_lbl = tk.Label(outer, text=title,
                             font=(FONTS['h3'][0], 11, 'bold'),
                             bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                             wraplength=240, justify='center')
        title_lbl.pack(padx=12)
        sub_lbl = tk.Label(outer, text=subtitle, font=FONTS['meta'],
                           bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                           wraplength=240, justify='center')
        sub_lbl.pack(padx=12, pady=(4, 18))
        widgets = {'outer': outer, 'icon': icon_lbl, 'title': title_lbl, 'sub': sub_lbl}
        def _select(_=None, k=key):
            path_var.set(k)
            _refresh_tiles()
            _refresh_panels()
        for w in (outer, icon_lbl, title_lbl, sub_lbl):
            w.bind('<Button-1>', _select)
        return widgets

    # Dump-direct PFS build was removed \u2014 only the stable exFAT/ffpkg \u2192 .ffpfsc
    # route remains (Convert), plus Extract.
    tile_convert = _make_path_tile(sel_frame, 'convert',
        '\U0001f504', 'Build from .exfat / .ffpkg',
        'Already have one? Pack it straight into a mountable .ffpfsc \u2014 no rebuild')
    tile_extract = _make_path_tile(sel_frame, 'extract',
        '\U0001f4e4', 'Extract a .ffpfs / .ffpfsc image',
        'Unpack a PFS image to a folder \u2014 edit then rebuild')

    def _refresh_tiles():
        for key, tile in (('convert', tile_convert),
                          ('extract', tile_extract)):
            active = path_var.get() == key
            tile['outer'].config(
                highlightbackground=COLORS['teal'] if active else COLORS['border_2'],
                bg=COLORS['bg_3'] if active else COLORS['bg_2'])
            for w in (tile['icon'], tile['title'], tile['sub']):
                w.config(bg=COLORS['bg_3'] if active else COLORS['bg_2'],
                         fg=(COLORS['teal'] if active and (w is tile['title'] or w is tile['icon'])
                             else COLORS['fg_1'] if w is tile['title']
                             else COLORS['fg_4']))
    _refresh_tiles()

    panel_build   = tk.Frame(inner, bg=COLORS['bg_1'])
    panel_convert = tk.Frame(inner, bg=COLORS['bg_1'])
    panel_extract = tk.Frame(inner, bg=COLORS['bg_1'])

    def _refresh_panels():
        for p in (panel_build, panel_convert, panel_extract):
            p.pack_forget()
        sel = path_var.get()
        if sel == 'extract':
            panel_extract.pack(fill='x')
        else:
            panel_convert.pack(fill='x')

    # ════════════════════════════════════════════════════════════════
    # BUILD — dump folder → .ffpfs (pack folder, the proven method)
    # ════════════════════════════════════════════════════════════════
    src_var       = tk.StringVar()
    outdir_var    = tk.StringVar()
    name_var      = tk.StringVar()
    size_var      = tk.StringVar(value='')
    fmt_var         = tk.StringVar(value='ffpfsc')  # 'ffpfsc' | 'ffpfs'
    version_ps5_var = tk.BooleanVar(value=True)
    tempdir_var   = tk.StringVar()   # optional custom temp/spool folder
    # Default to a balanced core count: half the machine's cores (min 2 if
    # available) so compression is fast out of the box without maxing the CPU.
    try:
        import multiprocessing as _mp0
        _def_cores = max(1, _mp0.cpu_count() // 2)
    except Exception:
        _def_cores = 1
    cpu_cores_var = tk.StringVar(value=str(_def_cores))  # compression CPU cores
    state_b       = {'busy': False}

    # Recommendation: dump-direct works for some games; exFAT/ffpkg-first is
    # near-universal (per the ShadowMount+ dev). Steer users to the reliable
    # two-step route without hiding the direct option.
    _rec_b = tk.Frame(panel_build, bg=COLORS['bg_3'],
                      highlightthickness=1, highlightbackground=COLORS['teal'])
    _rec_b.pack(fill='x', padx=24, pady=(0, 12))
    tk.Label(_rec_b,
             text=('\u2b50  Most reliable: build an .exfat (exFAT tab) or .ffpkg '
                   '(ffpkg tab) first, then use Convert to pack it into a .ffpfsc. '
                   'Only some games mount when built straight from a dump \u2014 almost '
                   'all work via exFAT/ffpkg \u2192 Convert.'),
             font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_2'],
             anchor='w', justify='left', wraplength=1080).pack(
                 fill='x', padx=12, pady=8)

    card_b = tk.Frame(panel_build, bg=COLORS['bg_2'],
                      highlightthickness=1, highlightbackground=COLORS['border_2'])
    card_b.pack(fill='x', padx=24, pady=(0, 14))
    _card_head(card_b, '\U0001f5c2', 'Dump folder  \u2192  .ffpfs / .ffpfsc',
               'Pick a format below \u2014 uncompressed (one pass) or compressed container')
    body_b = tk.Frame(card_b, bg=COLORS['bg_2'])
    body_b.pack(fill='both', expand=True, padx=24, pady=(4, 18))
    # Two-column layout (mirrors the exFAT/ffpkg tabs): inputs on the left,
    # cover + progress + queue on the right.
    body_b.grid_columnconfigure(0, weight=14, minsize=400)   # left  ~58%
    body_b.grid_columnconfigure(1, weight=10, minsize=300)   # right ~42%
    body_b.grid_rowconfigure(0, weight=1)
    left_b = tk.Frame(body_b, bg=COLORS['bg_2'])
    left_b.grid(row=0, column=0, sticky='new', padx=(0, 12))
    right_b = tk.Frame(body_b, bg=COLORS['bg_2'])
    right_b.grid(row=0, column=1, sticky='nsew', padx=(12, 0))

    def _browse_src_b():
        p = filedialog.askdirectory(title='Select game dump folder')
        if p:
            src_var.set(p)
    def _browse_out_b():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            outdir_var.set(p)

    field_block(left_b, 'Game dump folder', var=src_var, on_browse=_browse_src_b,
                hint='folder containing sce_sys/param.json and eboot.bin')
    field_block(left_b, 'Output folder', var=outdir_var, on_browse=_browse_out_b,
                hint='where the image will be written')
    field_block(left_b, 'Output name', var=name_var,
                hint='auto-filled from folder name; extension follows the format below')

    def _browse_temp_b():
        p = filedialog.askdirectory(title='Select temp / spool folder')
        if p:
            tempdir_var.set(p)
    field_block(left_b, 'Temp folder (optional)', var=tempdir_var,
                on_browse=_browse_temp_b,
                hint='leave blank for default; set only to redirect the spool off C:')

    def _on_src_b(*_):
        p = src_var.get().strip()
        if not p or not os.path.isdir(p):
            return
        if not outdir_var.get():
            outdir_var.set(p)
        base = os.path.basename(p.rstrip('/\\'))
        ext = '.ffpfs' if fmt_var.get() == 'ffpfs' else '.ffpfsc'
        # Update the name if it's empty OR was auto-filled from a previous
        # source/format (so picking a new image or format refreshes it).
        # Only a name the user typed themselves is preserved.
        cur = name_var.get().strip()
        if not cur or cur == state_b.get('auto_name'):
            new_name = base + ext
            name_var.set(new_name)
            state_b['auto_name'] = new_name
        size_var.set('Checking dump structure\u2026')
        def _calc():
            root = _find_game_root(p)
            sz = _get_folder_size(p)
            cnt = _count_files(p)
            if fmt_var.get() == 'ffpfs':
                est = 'PFS size \u2248 %s (uncompressed)' % _fmt_size(sz)
            else:
                est = 'PFS estimate ~%s\u2013%s (after compression)' % (
                    _fmt_size(sz * 0.55), _fmt_size(sz * 0.95))
            if root is None:
                msg = ('\u26a0  No sce_sys/param.json found \u2014 this may not be a valid '
                       'game dump.  %s \u00b7 %d files') % (_fmt_size(sz), cnt)
            elif os.path.normpath(root) != os.path.normpath(p):
                msg = ('\u2713  Game found in subfolder: %s\n%s \u00b7 %d files \u00b7 %s') % (
                    os.path.basename(root), _fmt_size(sz), cnt, est)
            else:
                msg = ('\u2713  Valid game dump  \u00b7  %s \u00b7 %d files\n%s') % (
                    _fmt_size(sz), cnt, est)
            # Cover art: load the PIL image on this worker thread (safe);
            # build the Tk PhotoImage on the main thread (Tk is not
            # thread-safe).
            cover_pil = None
            try:
                _icon = os.path.join(root, 'sce_sys', 'icon0.png') if root else None
                if _icon and os.path.isfile(_icon):
                    from exfat_builder import _load_cover_image
                    cover_pil = _load_cover_image(_icon, target=140)
            except Exception:
                cover_pil = None
            def _apply(m=msg, pil=cover_pil):
                size_var.set(m)
                try:
                    if pil is not None:
                        from PIL import ImageTk
                        photo = ImageTk.PhotoImage(pil)
                        state_b['cover_img'] = photo      # keep a ref vs GC
                        cover_lbl_b.config(image=photo, text='')
                    else:
                        state_b.pop('cover_img', None)
                        cover_lbl_b.config(image='', text='\U0001f3ae')
                    cover_lbl_b.pack(pady=(4, 4))
                except Exception:
                    pass
            parent.after(0, _apply)
        threading.Thread(target=_calc, daemon=True).start()
    src_var.trace_add('write', _on_src_b)

    size_lbl = tk.Label(left_b, textvariable=size_var, font=FONTS['mono_sm'],
                        bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w', justify='left')
    size_lbl.pack(fill='x', pady=(8, 4))

    # Cover art for the detected dump (icon0.png), in the right column.
    # Shown once a source is picked; controller glyph if there's no icon.
    cover_panel_b = tk.Frame(right_b, bg=COLORS['bg_3'],
                             highlightthickness=1,
                             highlightbackground=COLORS['border_3'])
    cover_panel_b.pack(fill='x', pady=(0, 10))
    tk.Label(cover_panel_b, text='COVER', font=FONTS['eyebrow'],
             bg=COLORS['bg_3'], fg=COLORS['fg_5']).pack(anchor='w',
             padx=12, pady=(8, 2))
    cover_row_b = tk.Frame(cover_panel_b, bg=COLORS['bg_3'])
    cover_row_b.pack(fill='x', pady=(0, 10))
    cover_lbl_b = tk.Label(cover_row_b, text='\U0001f3ae', bg=COLORS['bg_3'],
                           fg=COLORS['fg_5'], font=(FONTS['mono_sm'][0], 34))
    cover_lbl_b.pack(pady=(4, 4))

    # ── Output format ────────────────────────────────────────────────
    fmt_frame = tk.Frame(left_b, bg=COLORS['bg_2'])
    fmt_frame.pack(fill='x', pady=(10, 0))
    tk.Label(fmt_frame, text='Output format:', font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')

    def _mk_fmt_radio(value, text):
        rb = tk.Radiobutton(
            fmt_frame, text=text, value=value, variable=fmt_var,
            command=lambda: _apply_fmt(),
            font=FONTS['mono_sm'],
            bg=COLORS['bg_2'], fg=COLORS['fg_2'],
            selectcolor=COLORS['bg_4'],
            activebackground=COLORS['bg_2'], activeforeground=COLORS['fg_1'],
            highlightthickness=0, bd=0, cursor='hand2')
        rb.pack(side='left', padx=(12, 0))
        return rb
    _mk_fmt_radio('ffpfsc', '.ffpfsc (compressed)')
    _mk_fmt_radio('ffpfs',  '.ffpfs (uncompressed)')

    fmt_desc_var = tk.StringVar()
    tk.Label(left_b, textvariable=fmt_desc_var, font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w',
             justify='left').pack(fill='x', pady=(4, 0))

    def _apply_fmt():
        if fmt_var.get() == 'ffpfs':
            fmt_desc_var.set('Uncompressed PFS \u2014 larger file, full console read '
                             'speed, one fast pass (no temporary file).')
            try:
                btn_b.config(text='Build .ffpfs now')
            except Exception:
                pass
        else:
            fmt_desc_var.set('Compressed container \u2014 smaller on disk; console reads '
                             'capped ~150\u2013250 MB/s. Official ShadowMount+ method.')
            try:
                btn_b.config(text='Build .ffpfsc now')
            except Exception:
                pass
        if src_var.get().strip():
            _on_src_b()

    # ── Advanced: CPU cores for compression ──────────────────────────
    try:
        import multiprocessing as _mp
        _max_cores = max(1, _mp.cpu_count())
    except Exception:
        _max_cores = 1
    adv_b = tk.Frame(left_b, bg=COLORS['bg_2'])
    adv_b.pack(fill='x', pady=(10, 0))
    tk.Label(adv_b, text='CPU cores:', font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')
    _core_choices = [str(n) for n in (1, 2, 4, 6, 8, 12, 16) if n <= _max_cores] or ['1']
    if str(_max_cores) not in _core_choices:
        _core_choices.append(str(_max_cores))
    core_menu = ttk.Combobox(adv_b, textvariable=cpu_cores_var,
                              values=_core_choices, width=5, state='readonly')
    core_menu.pack(side='left', padx=(8, 0))
    tk.Label(adv_b,
             text=('  more cores = faster compression (you have %d). '
                   '1 is safest.' % _max_cores),
             font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_5']).pack(side='left')

    prog_b = _ProgressBlock(right_b, _STAGES_BUILD)

    btn_row_b = tk.Frame(left_b, bg=COLORS['bg_2'])
    btn_row_b.pack(anchor='w', pady=(14, 0))
    btn_b = make_themed_button(btn_row_b, text='Build .ffpfsc now',
                                command=lambda: _run_build(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=18, pady=9)
    btn_b.pack(side='left')
    _apply_fmt()   # set initial description + button label for default format
    btn_add_q = make_themed_button(btn_row_b, text='Add to queue',
                                command=lambda: _add_to_queue(),
                                kind='primary', icon='\u2795',
                                font_size=10, padx=16, pady=9)
    btn_add_q.pack(side='left', padx=(8, 0))

    # ── Queue state + UI ──────────────────────────────────────────────
    # Each job: dict(game_root, out_path, name, compress, version_ps5,
    #                 cpu_cores, temp_dir, status)
    queue = []          # list of job dicts
    queue_state = {'running': False, 'paused': False, 'cancel': False}

    queue_card = tk.Frame(right_b, bg=COLORS['bg_3'],
                          highlightthickness=1, highlightbackground=COLORS['border_3'])
    # packed/unpacked dynamically by _refresh_queue

    qhdr = tk.Frame(queue_card, bg=COLORS['bg_2'])
    qhdr.pack(fill='x')
    tk.Label(qhdr, text='BUILD QUEUE', font=FONTS['eyebrow'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w').pack(
                 side='left', padx=14, pady=8)
    qcount_var = tk.StringVar(value='')
    tk.Label(qhdr, textvariable=qcount_var, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['teal']).pack(side='right', padx=14)
    tk.Frame(queue_card, bg=COLORS['border_3'], height=1).pack(fill='x')

    qlist = tk.Frame(queue_card, bg=COLORS['bg_3'])
    qlist.pack(fill='x', padx=4, pady=4)

    qbtns = tk.Frame(queue_card, bg=COLORS['bg_3'])
    qbtns.pack(fill='x', padx=14, pady=(2, 12))
    btn_run_q = make_themed_button(qbtns, text='Build queue',
                                command=lambda: _run_queue(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=16, pady=8)
    btn_run_q.pack(side='left')
    btn_pause_q = make_themed_button(qbtns, text='Pause',
                                command=lambda: _toggle_pause(),
                                kind='ghost', icon='\u23f8',
                                font_size=10, padx=14, pady=8)
    btn_pause_q.pack(side='left', padx=(8, 0))
    btn_clear_q = make_themed_button(qbtns, text='Clear',
                                command=lambda: _clear_queue(),
                                kind='ghost', icon='\u2717',
                                font_size=10, padx=14, pady=8)
    btn_clear_q.pack(side='left', padx=(8, 0))
    btn_rmsel_q = make_themed_button(qbtns, text='Remove selected',
                                command=lambda: _remove_selected(),
                                kind='ghost', icon='\u2715',
                                font_size=10, padx=14, pady=8)
    btn_rmsel_q.pack(side='left', padx=(8, 0))
    qsd_var = tk.BooleanVar(value=False)
    qsd_cb = _make_cb(qbtns, 'Shut down PC when queue finishes', qsd_var)
    qsd_cb.pack(side='left', padx=(16, 0))

    _STATUS_COLORS = {
        'queued':  COLORS['fg_4'],
        'running': COLORS['teal'],
        'done':    COLORS['success'],
        'failed':  COLORS['danger'],
    }
    _STATUS_DOT = {'queued': '\u25cb', 'running': '\u25cf',
                   'done': '\u2713', 'failed': '\u2717'}

    _sel_vars = {}   # id(job) -> BooleanVar for multi-select checkboxes

    def _refresh_queue():
        for w in qlist.winfo_children():
            w.destroy()
        if not queue:
            queue_card.pack_forget()
            qcount_var.set('')
            _sel_vars.clear()
            return
        queue_card.pack(fill='both', expand=True, pady=(0, 0))
        done = sum(1 for j in queue if j['status'] == 'done')
        qcount_var.set('%d job%s  \u00b7  %d done'
                       % (len(queue), '' if len(queue) == 1 else 's', done))
        # Drop selection state for jobs that are no longer in the queue
        _present = {id(j) for j in queue}
        for k in [k for k in _sel_vars if k not in _present]:
            del _sel_vars[k]
        running = queue_state['running']
        n = len(queue)
        for idx, job in enumerate(queue):
            editable = (job['status'] == 'queued' and not running)
            row = tk.Frame(qlist, bg=COLORS['bg_3'])
            row.pack(fill='x', padx=10, pady=2)
            # Multi-select checkbox (only on queued rows, when idle)
            if editable:
                var = _sel_vars.get(id(job))
                if var is None:
                    var = tk.BooleanVar(value=False)
                    _sel_vars[id(job)] = var
                tk.Checkbutton(row, variable=var, bg=COLORS['bg_3'],
                               activebackground=COLORS['bg_3'],
                               selectcolor=COLORS['bg_4'],
                               highlightthickness=0, bd=0,
                               cursor='hand2').pack(side='left')
            else:
                tk.Frame(row, bg=COLORS['bg_3'], width=20,
                         height=1).pack(side='left')
            dot = tk.Label(row, text=_STATUS_DOT.get(job['status'], '\u25cb'),
                           font=(FONTS['mono_sm'][0], 9), bg=COLORS['bg_3'],
                           fg=_STATUS_COLORS.get(job['status'], COLORS['fg_4']))
            dot.pack(side='left', padx=(2, 8))
            tk.Label(row, text=job['name'], font=FONTS['mono_sm'],
                     bg=COLORS['bg_3'], fg=COLORS['fg_1'], anchor='w').pack(
                         side='left', fill='x', expand=True)
            st = tk.Label(row, text=job['status'], font=FONTS['meta'],
                          bg=COLORS['bg_3'],
                          fg=_STATUS_COLORS.get(job['status'], COLORS['fg_4']))
            st.pack(side='right')
            if editable:
                # Single remove
                x = tk.Label(row, text='\u2715', font=FONTS['meta'],
                             bg=COLORS['bg_3'], fg=COLORS['fg_5'], cursor='hand2')
                x.pack(side='right', padx=(0, 10))
                x.bind('<Button-1>', lambda e, j=job: _remove_job(j))
                # Reorder down / up (hidden at the ends)
                if idx < n - 1:
                    dn = tk.Label(row, text='\u25bc', font=FONTS['meta'],
                                  bg=COLORS['bg_3'], fg=COLORS['fg_4'], cursor='hand2')
                    dn.pack(side='right', padx=(0, 6))
                    dn.bind('<Button-1>', lambda e, i=idx: _move(i, +1))
                if idx > 0:
                    up = tk.Label(row, text='\u25b2', font=FONTS['meta'],
                                  bg=COLORS['bg_3'], fg=COLORS['fg_4'], cursor='hand2')
                    up.pack(side='right', padx=(0, 6))
                    up.bind('<Button-1>', lambda e, i=idx: _move(i, -1))

    def _remove_job(job):
        if queue_state['running']:
            return
        try:
            queue.remove(job)
        except ValueError:
            pass
        _sel_vars.pop(id(job), None)
        _refresh_queue()

    def _move(i, delta):
        if queue_state['running']:
            return
        j = i + delta
        if 0 <= j < len(queue):
            queue[i], queue[j] = queue[j], queue[i]
            _refresh_queue()

    def _remove_selected():
        if queue_state['running']:
            return
        victims = [j for j in queue
                   if id(j) in _sel_vars and _sel_vars[id(j)].get()]
        for j in victims:
            try:
                queue.remove(j)
            except ValueError:
                pass
            _sel_vars.pop(id(j), None)
        _refresh_queue()

    def _validate_job():
        """Read the current form into a validated job dict, or None."""
        src    = src_var.get().strip()
        outdir = outdir_var.get().strip()
        name   = name_var.get().strip()
        if not src or not os.path.isdir(src):
            messagebox.showerror('Source missing', 'Pick a valid game dump folder.')
            return None
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Output folder missing', 'Pick an output folder.')
            return None
        if not name:
            messagebox.showerror('Output name missing', 'Set an output filename.')
            return None
        # Output extension follows the chosen format.
        fmt = fmt_var.get()
        want_ext = '.ffpfs' if fmt == 'ffpfs' else '.ffpfsc'
        low = name.lower()
        if low.endswith('.ffpfsc'):
            name = name[:-7] + want_ext
        elif low.endswith('.ffpfs'):
            name = name[:-6] + want_ext
        else:
            name += want_ext
        game_root = _find_game_root(src)
        if game_root is None:
            if not messagebox.askyesno('No game files found',
                    'sce_sys/param.json was not found in this folder or one level '
                    'down.\n\nThe resulting image may not mount on the PS5.\n\n'
                    'Continue anyway?'):
                return None
            game_root = src
        out_path = os.path.normpath(os.path.join(outdir, name))

        # Block stray image files in the dump. mkpfs 'pack folder' has no
        # exclude option, so a leftover .ffpfsc/.exfat/etc. in the dump would
        # be packed into the new image (huge bloat) or crash the pack on a
        # non-ASCII name. These are almost always previous build outputs that
        # don't belong in a game dump. (Top-level scan: instant, and that's
        # where stray outputs land.)
        _img_exts = ('.ffpfs', '.ffpfsc', '.exfat', '.ffpkg')
        strays = []
        try:
            for _e in os.scandir(game_root):
                if _e.is_file() and _e.name.lower().endswith(_img_exts):
                    strays.append(_e.name)
        except Exception:
            pass
        if strays:
            _shown = '\n'.join('  \u2022 ' + s for s in strays[:6])
            messagebox.showerror('Image files in the dump',
                'The dump folder contains image file(s) that should not be '
                'packed:\n\n%s\n\nThese are usually leftover builds. Packing '
                'them bloats the image (or crashes on a non-ASCII name). '
                'Move them out of the dump folder and try again.' % _shown)
            return None

        # Block writing the output INTO the source dump — it would then be
        # packed into the next build (the stray-image trap above).
        try:
            _src_abs = os.path.abspath(game_root)
            _out_abs = os.path.abspath(out_path)
            if os.path.commonpath([_src_abs, _out_abs]) == _src_abs:
                messagebox.showerror('Output inside the dump',
                    'The output would be written inside the source dump '
                    'folder:\n\n%s\n\nThat image would get packed into your '
                    'next build. Choose an output folder outside the dump.'
                    % out_path)
                return None
        except Exception:
            pass

        try:
            _cores = int(cpu_cores_var.get())
        except Exception:
            _cores = 1
        return {
            'game_root': game_root,
            'out_path':  out_path,
            'name':      name,
            'outdir':    outdir,
            'work_dir':  outdir,
            'fmt':       fmt,
            'version_ps5': bool(version_ps5_var.get()),
            'cpu_cores': max(1, _cores),
            'temp_dir':  tempdir_var.get().strip(),
            'status':    'queued',
        }

    def _add_to_queue():
        if queue_state['running']:
            messagebox.showinfo('Queue running',
                'The queue is currently building. Add jobs after it finishes, '
                'or pause first.')
            return
        job = _validate_job()
        if not job:
            return
        # Warn on duplicate output path
        for j in queue:
            if os.path.normpath(j['out_path']) == os.path.normpath(job['out_path']):
                if not messagebox.askyesno('Already queued',
                        job['name'] + ' is already in the queue. Add again?'):
                    return
                break
        queue.append(job)
        _refresh_queue()
        app._log('[PFS] Queued: %s\n' % job['name'])

    # ── Core build routine, shared by single build and queue ──────────
    def _build_job(job, on_finish):
        """Build one PFS image from a dump folder.

          .ffpfs  (uncompressed): one pass —
            mkpfs pack folder --no-compress ... <dump> out.ffpfs
          .ffpfsc (compressed): the official ShadowMount+ two-step —
            1. mkpfs pack folder --no-compress ... <dump> pfs_image.dat
            2. mkpfs pack file ... pfs_image.dat out.ffpfsc
          (the temporary pfs_image.dat is removed afterwards)

        Calls on_finish(success: bool) on the Tk thread when complete.
        """
        prog_b.reset()
        prog_b.update('pack', 0, 'Starting\u2026')

        # Live file-size poller: while a step is writing, watch the target
        # file grow on disk and show it in the detail line. mkpfs goes silent
        # during the long write phase, so this gives real-time feedback.
        poll = {'target': None, 'label': '', 'total': 0, 'on': True}

        def _poll_size():
            if not poll['on']:
                return
            tgt = poll['target']
            if tgt:
                sz = _live_file_size(tgt)
                if sz:
                    if poll['total']:
                        pct = min(99, int(sz * 100 / poll['total']))
                        prog_b.set_detail('%s  %s / ~%s  (%d%%)'
                            % (poll['label'], _fmt_size(sz),
                               _fmt_size(poll['total']), pct))
                    else:
                        prog_b.set_detail('%s  %s written\u2026'
                            % (poll['label'], _fmt_size(sz)))
            parent.after(1000, _poll_size)

        def _log(l):
            parent.after(0, lambda x=str(l): app._log('[PFS] ' + x + '\n'))

        def worker():
            stats = {}
            disk_full = {'hit': False}
            _tmp = None
            _markers = []
            nested = None       # temporary pfs_image.dat
            ok = False
            # Estimate the uncompressed nested size (~ dump size) for the
            # step-1 progress %. Step 2 (compressed) total is unknown.
            try:
                dump_sz = _get_folder_size(job['game_root'])
            except Exception:
                dump_sz = 0
            parent.after(0, _poll_size)   # start the live size poller
            def _log_parse(l):
                _log(l)
                _parse_summary(l, stats)
                low = str(l).lower()
                if 'no space left' in low or 'errno 28' in low:
                    disk_full['hit'] = True
                # mkpfs emits plain status lines (not [###] bars) during the
                # write/discovery phases. Reflect them so the UI never looks
                # frozen on a large image.
                if 'writing pfs image' in low:
                    parent.after(0, lambda: prog_b.update('pack', 95,
                        'Writing uncompressed PFS (large games take several min)\u2026'))
                elif 'discovering files' in low:
                    parent.after(0, lambda: prog_b.update('pack', 2,
                        'Discovering files\u2026'))
                elif low.startswith('reading') or ' reading ' in low:
                    parent.after(0, lambda: prog_b.update('pack', 10,
                        'Reading source files\u2026'))

            def _on_progress(phase, pct, total, detail):
                parent.after(0, lambda ph=phase, p=pct, d=detail:
                    prog_b.update(ph, p, d))

            try:
                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    parent.after(0, lambda: (
                        prog_b.fail(),
                        prog_b.set_status('mkpfs not available', error=True)))
                    return

                out_path = job['out_path']            # ....ffpfs / ....ffpfsc
                work_dir = job.get('work_dir') or os.path.dirname(out_path)
                uncompressed = (job.get('fmt') == 'ffpfs')

                if uncompressed:
                    # pack folder --no-compress writes the standalone .ffpfs
                    # directly — no nested file, no second pass.
                    nested  = None
                    target1 = out_path
                else:
                    # IMPORTANT: no leading dot. mkpfs 'pack file' records the
                    # source basename as the nested filename, and ShadowMount+
                    # only treats a nested file named exactly 'pfs_image.dat'
                    # as a PFS image. A dotted name mounts as an empty container.
                    nested  = os.path.normpath(os.path.join(work_dir, 'pfs_image.dat'))
                    target1 = nested

                # Overwrite existing output / stale nested file
                for p in (out_path, nested):
                    if p and os.path.exists(p):
                        try:
                            os.remove(p)
                        except Exception:
                            pass

                # Temp-folder redirect (only when user set one)
                ct = job.get('temp_dir', '')
                tmp_args = []
                if ct and os.path.isdir(ct):
                    _tmp = os.path.join(ct, '.mkpfs_tmp')
                    try:
                        os.makedirs(_tmp, exist_ok=True)
                        tmp_args = ['--temp-folder', _tmp]
                    except Exception:
                        _tmp = None

                # Preserve empty dirs in the source (mkpfs drops them)
                _markers = _seed_empty_dir_markers(job['game_root'])
                if _markers:
                    _log('Preserved %d empty director%s with .pfskeep markers'
                         % (len(_markers), 'y' if len(_markers) == 1 else 'ies'))

                # ── STEP 1: pack the dump into a PFS (pack folder) ─────
                # Exact flags per ShadowMount+ README. --no-compress always:
                # for .ffpfs this IS the output; for .ffpfsc it's the
                # uncompressed nested image that step 2 then compresses.
                step_lbl = ('Packing uncompressed PFS\u2026' if uncompressed
                            else 'Step 1/2: packing uncompressed PFS\u2026')
                argv1 = ['pack', 'folder', '--no-compress',
                         '--no-adjust-output-file-extension',
                         '--version', 'PS5', '--inode-bits', '32',
                         '--cpu-count', str(job['cpu_cores'])]
                argv1 += tmp_args
                argv1 += [job['game_root'], target1]
                _log(('Pack \u2014 mkpfs ' if uncompressed else 'Step 1/2 \u2014 mkpfs ')
                     + ' '.join(argv1))
                poll['target'] = target1
                poll['label']  = 'Packing PFS'
                poll['total']  = dump_sz
                parent.after(0, lambda l=step_lbl: prog_b.update('pack', 0, l))

                rc1 = run_mkpfs(argv1, log_cb=_log_parse, progress_cb=_on_progress)
                if not (rc1 == 0 or (rc1 == 1 and os.path.exists(target1))) \
                        or not os.path.exists(target1):
                    if disk_full['hit']:
                        parent.after(0, lambda: (
                            prog_b.fail(),
                            prog_b.set_status('Out of disk space', error=True)))
                    else:
                        parent.after(0, lambda: (
                            prog_b.fail(),
                            prog_b.set_status(
                                ('Pack failed (rc=%d)' if uncompressed
                                 else 'Step 1 failed (rc=%d)') % rc1, error=True)))
                    return

                if uncompressed:
                    # Done — the .ffpfs is the step-1 output.
                    rc2     = rc1
                    actual  = out_path
                    file_ok = os.path.exists(actual)
                else:
                    # ── STEP 2: compress nested into .ffpfsc (pack file) ───
                    # pack file stages the source via a hard link, which fails on
                    # exFAT/FAT32 temp drives. If our temp dir can't hard-link,
                    # omit --temp-folder so mkpfs stages on the system temp (NTFS).
                    argv2 = ['pack', 'file',
                             '--version', 'PS5', '--inode-bits', '32',
                             '--cpu-count', str(job['cpu_cores'])]
                    step2_tmp = tmp_args
                    if tmp_args:
                        if not _supports_hardlinks(_tmp):
                            _log('Temp folder does not support hard links '
                                 '(exFAT/FAT32?) \u2014 using system temp for staging.')
                            step2_tmp = []
                    argv2 += step2_tmp
                    argv2 += [nested, out_path]
                    _log('Step 2/2 \u2014 mkpfs ' + ' '.join(argv2))
                    poll['target'] = out_path
                    poll['label']  = 'Compressing'
                    poll['total']  = 0   # compressed final size unknown up front
                    parent.after(0, lambda: prog_b.update('compress', 0,
                        'Step 2/2: compressing into .ffpfsc\u2026'))

                    rc2 = run_mkpfs(argv2, log_cb=_log_parse, progress_cb=_on_progress)

                    # mkpfs 'pack file' always writes the .ffpfsc. Accept only
                    # that exact output \u2014 it was removed before packing, so its
                    # presence means THIS run wrote it. Never fall back to a stale
                    # .ffpfs from an earlier build (which was a false success).
                    actual = out_path
                    file_ok = os.path.exists(actual)

                if rc2 == 0 or (rc2 == 1 and file_ok):
                    ok = True
                    sz = os.path.getsize(actual) / 1024**3
                    parent.after(0, lambda s=dict(stats), _sz=sz, _act=actual: (
                        prog_b.done(),
                        prog_b.set_status('Done \u2713  %.2f GB' % _sz),
                        _show_summary_panel(panel_build, s, _act)))
                elif disk_full['hit']:
                    parent.after(0, lambda: (
                        prog_b.fail(),
                        prog_b.set_status('Out of disk space', error=True)))
                    try:
                        if os.path.exists(out_path):
                            os.remove(out_path)
                    except Exception:
                        pass
                else:
                    parent.after(0, lambda: (
                        prog_b.fail(),
                        prog_b.set_status('Step 2 failed (rc=%d) \u2014 see OUTPUT LOG' % rc2, error=True)))
                    try:
                        if os.path.exists(out_path):
                            os.remove(out_path)
                    except Exception:
                        pass
            except Exception as e:
                _log('Build error: ' + str(e))
                parent.after(0, lambda e=e: (
                    prog_b.fail(),
                    prog_b.set_status('Error: ' + str(e)[:60], error=True)))
            finally:
                poll['on'] = False   # stop the live size poller
                _remove_markers(_markers)
                # Remove the temporary nested PFS image
                try:
                    if nested and os.path.exists(nested):
                        os.remove(nested)
                        _log('Removed temporary nested image')
                except Exception:
                    pass
                try:
                    if _tmp and os.path.isdir(_tmp):
                        shutil.rmtree(_tmp, ignore_errors=True)
                except Exception:
                    pass
                parent.after(0, lambda: on_finish(ok))

        threading.Thread(target=worker, daemon=True).start()

    # ── Single immediate build ────────────────────────────────────────
    def _run_build():
        if state_b['busy'] or queue_state['running']:
            return
        job = _validate_job()
        if not job:
            return
        if os.path.exists(job['out_path']):
            if not messagebox.askyesno('Overwrite',
                    job['out_path'] + '\n\nalready exists. Overwrite?'):
                return
        # Free-space check
        try:
            need = _get_folder_size(job['game_root'])
            free = _free_space(job['outdir'])
            if free is not None and free < need * 0.95:
                if not messagebox.askyesno('Low disk space',
                        'Output drive free: %s\nDump size: %s\n\n'
                        'You may run out of space. Continue anyway?'
                        % (_fmt_size(free), _fmt_size(need))):
                    return
        except Exception:
            pass

        state_b['busy'] = True
        btn_b.config(state='disabled')
        btn_add_q.config(state='disabled')

        def _finish(ok):
            state_b['busy'] = False
            btn_b.config(state='normal')
            btn_add_q.config(state='normal')
            try:
                app._trigger_shutdown('pfs')
            except Exception:
                pass
            if ok:
                messagebox.showinfo('Done', 'PFS image written:\n' + job['out_path'])
            else:
                messagebox.showerror('Build failed',
                    'The build did not complete. Check the OUTPUT LOG.')
        _build_job(job, _finish)

    # ── Queue runner ──────────────────────────────────────────────────
    def _toggle_pause():
        if not queue_state['running']:
            return
        queue_state['paused'] = not queue_state['paused']
        btn_pause_q.config(text='Resume' if queue_state['paused'] else 'Pause')
        app._log('[PFS] Queue %s\n'
                 % ('paused' if queue_state['paused'] else 'resumed'))

    def _clear_queue():
        if queue_state['running']:
            if not messagebox.askyesno('Stop queue',
                    'A build is running. Stop after the current job and clear '
                    'the rest?'):
                return
            queue_state['cancel'] = True
            # Remove everything still queued
            queue[:] = [j for j in queue if j['status'] == 'running']
            _refresh_queue()
            return
        queue.clear()
        _refresh_queue()

    def _run_queue():
        if queue_state['running']:
            return
        pending = [j for j in queue if j['status'] in ('queued', 'failed')]
        if not pending:
            messagebox.showinfo('Queue empty',
                'Add some jobs to the queue first.')
            return
        # Reset failed back to queued for a retry run
        for j in queue:
            if j['status'] == 'failed':
                j['status'] = 'queued'
        queue_state['running'] = True
        queue_state['cancel'] = False
        queue_state['paused'] = False
        btn_b.config(state='disabled')
        btn_add_q.config(state='disabled')
        btn_run_q.config(state='disabled')
        app._log('[PFS] Queue started: %d job(s)\n'
                 % sum(1 for j in queue if j['status'] == 'queued'))
        _refresh_queue()
        _next_in_queue()

    def _next_in_queue():
        # Honour pause
        if queue_state['paused']:
            parent.after(500, _next_in_queue)
            return
        if queue_state['cancel']:
            _finish_queue()
            return
        nxt = next((j for j in queue if j['status'] == 'queued'), None)
        if nxt is None:
            _finish_queue()
            return
        nxt['status'] = 'running'
        _refresh_queue()
        app._log('[PFS] Building queue item: %s\n' % nxt['name'])

        def _on_job_done(ok, job=nxt):
            job['status'] = 'done' if ok else 'failed'
            _refresh_queue()
            # Small gap so the UI updates, then continue
            parent.after(300, _next_in_queue)
        _build_job(nxt, _on_job_done)

    def _finish_queue():
        if not queue_state['running']:
            return  # already finished (e.g. Clear fired it, then a job completed)
        queue_state['running'] = False
        queue_state['paused'] = False
        btn_b.config(state='normal')
        btn_add_q.config(state='normal')
        btn_run_q.config(state='normal')
        btn_pause_q.config(text='Pause')
        done = sum(1 for j in queue if j['status'] == 'done')
        failed = sum(1 for j in queue if j['status'] == 'failed')
        _refresh_queue()
        app._log('[PFS] Queue finished: %d done, %d failed\n' % (done, failed))

        # Shutdown if the user armed the per-queue checkbox OR the global
        # PFS shutdown setting is on.
        armed = qsd_var.get()
        if armed:
            # Use the app's shutdown machinery if available, else a direct call
            try:
                # Temporarily mimic the trigger: respect global action/delay
                action = app._settings.get('shutdown_action', 'shutdown')
                if action == 'none':
                    action = 'shutdown'
                delay = int(app._settings.get('shutdown_delay', 60))
                if hasattr(app, '_shutdown_countdown') and delay > 0:
                    labels = {'shutdown': 'Shut down', 'restart': 'Restart',
                              'sleep': 'Sleep'}
                    app._shutdown_countdown(action, labels.get(action, action), delay)
                elif hasattr(app, '_execute_shutdown'):
                    app._execute_shutdown(action)
            except Exception as e:
                app._log('[PFS] Shutdown error: ' + str(e) + '\n')
        else:
            try:
                app._trigger_shutdown('pfs')
            except Exception:
                pass
            if failed == 0:
                messagebox.showinfo('Queue complete',
                    'All %d PFS image(s) built successfully.' % done)
            else:
                messagebox.showwarning('Queue complete',
                    '%d built, %d failed. Check the OUTPUT LOG.' % (done, failed))

    _refresh_queue()

    # ════════════════════════════════════════════════════════════════
    # CONVERT — .exfat / .ffpkg \u2192 .ffpfsc (single 'pack file', no extraction)
    # ════════════════════════════════════════════════════════════════
    src_c_var     = tk.StringVar()
    outdir_c_var  = tk.StringVar()
    name_c_var    = tk.StringVar()
    tempdir_c_var = tk.StringVar()
    info_c_var    = tk.StringVar(value='')
    cpu_cores_c_var   = tk.StringVar(value=str(_def_cores))
    verify_c_var      = tk.BooleanVar(value=False)
    state_c       = {'busy': False}
    convert_queue = []                       # list of job dicts
    cq_state      = {'running': False, 'idx': 0, 'sel': {}}

    card_c = tk.Frame(panel_convert, bg=COLORS['bg_2'],
                      highlightthickness=1, highlightbackground=COLORS['border_2'])
    card_c.pack(fill='x', padx=24, pady=(0, 14))
    _card_head(card_c, '\U0001f504', 'Build .ffpfsc from .exfat / .ffpkg',
               'Pick an existing image \u2014 it is packed straight into a compressed PFS container, no extraction')
    body_c = tk.Frame(card_c, bg=COLORS['bg_2'])
    body_c.pack(fill='x', padx=24, pady=(4, 18))

    note_c = tk.Frame(body_c, bg=COLORS['bg_3'],
                      highlightthickness=1, highlightbackground=COLORS['border_3'])
    note_c.pack(fill='x', pady=(0, 12))
    tk.Label(note_c,
             text=('\u2139  The .exfat / .ffpkg becomes the nested image inside the '
                   '.ffpfsc \u2014 one pack, no extraction, no OSFMount/UFS2Tool.\n'
                   'Official MicroMount method. Compression cuts most games '
                   '40\u201360%; reads top out around 150\u2013250 MB/s on console.\n'
                   'Have a raw dump folder? Build an .exfat (exFAT tab) or .ffpkg '
                   '(ffpkg tab) first \u2014 that is the supported route into a .ffpfsc.'),
             font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_3'],
             anchor='w', justify='left').pack(fill='x', padx=12, pady=8)

    def _browse_src_c():
        p = filedialog.askopenfilename(title='Select source image',
            filetypes=[('Game images', '*.exfat *.ffpkg'),
                       ('exFAT images', '*.exfat'),
                       ('ffpkg images', '*.ffpkg'),
                       ('All files', '*.*')])
        if p:
            src_c_var.set(p)
    def _browse_out_c():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            outdir_c_var.set(p)
    def _browse_temp_c():
        p = filedialog.askdirectory(title='Select temp / work folder')
        if p:
            tempdir_c_var.set(p)

    field_block(body_c, 'Source image (.exfat or .ffpkg)', var=src_c_var,
                on_browse=_browse_src_c, hint='the .exfat / .ffpkg to pack')
    field_block(body_c, 'Output folder', var=outdir_c_var,
                on_browse=_browse_out_c, hint='where the .ffpfsc will be written')
    field_block(body_c, 'Output name', var=name_c_var,
                hint='auto-filled from source name if blank')
    field_block(body_c, 'Temp / work folder (optional)', var=tempdir_c_var,
                on_browse=_browse_temp_c,
                hint='only used if the source drive can\u2019t hard-link '
                     '(exFAT/FAT32) \u2014 needs room for one image copy')

    def _on_src_c(*_):
        p = src_c_var.get().strip()
        if not p or not os.path.isfile(p):
            return
        if not outdir_c_var.get():
            outdir_c_var.set(os.path.dirname(p))
        if not name_c_var.get():
            name_c_var.set(os.path.splitext(os.path.basename(p))[0] + '.ffpfsc')
        try:
            sz = os.path.getsize(p)
            ext = os.path.splitext(p)[1].lower()
            if ext in ('.exfat', '.ffpkg'):
                info_c_var.set('Image: %s  \u00b7  packs directly into .ffpfsc '
                               '(no extraction)' % _fmt_size(sz))
            else:
                info_c_var.set('Image: %s' % _fmt_size(sz))
        except Exception:
            info_c_var.set('')
    src_c_var.trace_add('write', _on_src_c)

    # Cross-tab hand-off: let other tabs (e.g. exFAT/ffpkg after a build) drop
    # straight into Convert with the freshly built image pre-filled.
    def _open_convert_with(path=None):
        try:
            path_var.set('convert')
            _refresh_tiles()
            _refresh_panels()
            if path:
                src_c_var.set(path)   # _on_src_c auto-fills output dir + name
        except Exception:
            pass
    app._pfs_open_convert_with = _open_convert_with

    tk.Label(body_c, textvariable=info_c_var, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w').pack(fill='x', pady=(4, 8))

    space_c_var = tk.StringVar()
    space_c_lbl = tk.Label(body_c, textvariable=space_c_var, font=FONTS['meta'],
                           bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w',
                           justify='left')
    space_c_lbl.pack(fill='x', pady=(0, 6))

    def _update_space_c(*_):
        p = src_c_var.get().strip()
        if not p or not os.path.isfile(p):
            space_c_var.set('')
            return
        try:
            import tempfile
            need = os.path.getsize(p)
        except Exception:
            space_c_var.set('')
            return
        parts, short = [], False
        # Output drive needs up to ~source size (worst case: no compression).
        outdir = outdir_c_var.get().strip() or os.path.dirname(p)
        of = _free_space(outdir)
        odrv = os.path.splitdrive(os.path.abspath(outdir))[0] or outdir
        if of is not None:
            ok = of >= need
            short = short or not ok
            parts.append('Output (%s): need ~%s, %s free %s'
                         % (odrv, _fmt_size(need), _fmt_size(of),
                            '\u2713' if ok else '\u26a0'))
        # Temp/staging only needs a copy if it's a different drive than the
        # source, or the source FS can't hard-link (exFAT/FAT32).
        stage = tempdir_c_var.get().strip() or tempfile.gettempdir()
        sdrv  = os.path.splitdrive(os.path.abspath(stage))[0] or stage
        sd    = os.path.splitdrive(os.path.abspath(p))[0].lower()
        _chk  = stage if os.path.isdir(stage) else (
            os.path.splitdrive(os.path.abspath(stage))[0] + os.sep)
        try:
            will_copy = (sd != sdrv.lower()) or not _supports_hardlinks(_chk)
        except Exception:
            will_copy = True
        if will_copy:
            tf = _free_space(_chk)
            if tf is not None:
                ok = tf >= need
                short = short or not ok
                parts.append('Temp (%s): copy needs ~%s, %s free %s'
                             % (sdrv, _fmt_size(need), _fmt_size(tf),
                                '\u2713' if ok else '\u26a0'))
        else:
            parts.append('Temp (%s): hard-link \u2014 no copy needed \u2713' % sdrv)
        space_c_var.set('   \u00b7   '.join(parts))
        space_c_lbl.config(fg=COLORS['warn'] if short else COLORS['fg_4'])

    outdir_c_var.trace_add('write', _update_space_c)
    tempdir_c_var.trace_add('write', _update_space_c)
    src_c_var.trace_add('write', _update_space_c)

    adv_c = tk.Frame(body_c, bg=COLORS['bg_2'])
    adv_c.pack(fill='x', pady=(10, 0))
    tk.Label(adv_c, text='CPU cores:', font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')
    core_menu_c = ttk.Combobox(adv_c, textvariable=cpu_cores_c_var,
                                values=_core_choices, width=5, state='readonly')
    core_menu_c.pack(side='left', padx=(8, 0))
    tk.Label(adv_c, text='  more cores = faster compression (you have %d)' % _max_cores,
             font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_5']).pack(side='left')

    _make_cb(body_c, 'Verify after packing \u2014 re-reads the image to confirm it '
             'wrote correctly (slower; does not change the output)',
             verify_c_var).pack(anchor='w', pady=(8, 0))
    tk.Label(body_c, text=('   note: a converted image is nested, so verify may '
                           'log "sce_sys/param.json / eboot.bin not found" \u2014 '
                           'that is expected and does not mean the image is bad.'),
             font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_5'],
             anchor='w', justify='left').pack(fill='x', pady=(2, 0))

    _STAGES_CONVERT2 = [
        ('prep',     'Prepare',   0,   5),
        ('scan',     'Scan',      5,  15),
        ('compress', 'Compress', 15,  92),
        ('write',    'Write',    92, 100),
    ]
    prog_c = _ProgressBlock(body_c, _STAGES_CONVERT2)

    btn_row_c = tk.Frame(body_c, bg=COLORS['bg_2'])
    btn_row_c.pack(anchor='w', pady=(14, 0))
    btn_c = make_themed_button(btn_row_c, text='Build .ffpfsc now',
                                command=lambda: _run_convert(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=18, pady=9)
    btn_c.pack(side='left')
    btn_cq_add = make_themed_button(btn_row_c, text='Add to queue',
                                command=lambda: _cq_add(),
                                kind='accent', icon='\u002b',
                                font_size=10, padx=16, pady=9)
    btn_cq_add.pack(side='left', padx=(8, 0))
    btn_cq_run = make_themed_button(btn_row_c, text='Build all',
                                command=lambda: _cq_run_all(),
                                kind='teal', icon='\u25b6',
                                font_size=10, padx=16, pady=9)
    btn_cq_run.pack(side='left', padx=(8, 0))

    def _run_convert():
        if state_c['busy']:
            return
        src    = src_c_var.get().strip()
        outdir = outdir_c_var.get().strip()
        name   = name_c_var.get().strip()
        if not src or not os.path.isfile(src):
            messagebox.showerror('Source missing', 'Pick a valid .exfat or .ffpkg image.')
            return
        ext = os.path.splitext(src)[1].lower()
        if ext not in ('.exfat', '.ffpkg'):
            messagebox.showerror('Unsupported', 'Source must be a .exfat or .ffpkg image.')
            return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Output folder missing', 'Pick an output folder.')
            return
        if not name:
            messagebox.showerror('Output name missing', 'Set an output filename.')
            return
        if name.lower().endswith('.ffpfs'):
            name = name[:-6] + '.ffpfsc'
        elif not name.lower().endswith('.ffpfsc'):
            name += '.ffpfsc'
        out_path = os.path.normpath(os.path.join(outdir, name))

        # Both .exfat and .ffpkg pack straight into the .ffpfsc as a
        # nested image — the official MicroMount/MkPFS method is a single
        # 'mkpfs pack file' on either format. No extraction.

        # Work folder (only used as a staging fallback when the source
        # drive can't hard-link)
        work_base = tempdir_c_var.get().strip() or outdir
        work_dir  = os.path.normpath(os.path.join(work_base, '.pfs_convert_work'))

        if os.path.exists(out_path):
            if not cq_state['running']:
                if not messagebox.askyesno('Overwrite',
                        out_path + '\n\nalready exists. Overwrite?'):
                    return
            try:
                os.remove(out_path)
            except Exception:
                pass

        # Free-space pre-flight: output drive needs roughly the source
        # size (compressed PFS is usually <= source, worst case ~= source
        # + 64 KB padding per file)
        try:
            src_size = os.path.getsize(src)
            free = _free_space(outdir)
            if free is not None and free < src_size and not cq_state['running']:
                if not messagebox.askyesno('Low disk space',
                        'Output drive free: %s\nSource image: %s\n\n'
                        'The .ffpfsc is usually a bit smaller than the '
                        'source, but a poorly-compressing game may not '
                        'fit. Continue anyway?'
                        % (_fmt_size(free), _fmt_size(src_size))):
                    return
        except Exception:
            pass

        # Free-space pre-flight: temp/staging drive. 'pack file' stages the
        # source into the temp folder; if that's on a different drive than the
        # source (or the source filesystem can't hard-link, e.g. exFAT/FAT32),
        # it stages a full COPY and needs source-sized free space there.
        try:
            import tempfile as _tf_pf
            src_size  = os.path.getsize(src)
            stage_dir = tempdir_c_var.get().strip() or _tf_pf.gettempdir()
            _check_dir = stage_dir if os.path.isdir(stage_dir) else (
                os.path.splitdrive(os.path.abspath(stage_dir))[0] + os.sep)
            src_drv   = os.path.splitdrive(os.path.abspath(src))[0].lower()
            stage_drv = os.path.splitdrive(os.path.abspath(stage_dir))[0].lower()
            will_copy = (src_drv != stage_drv) or not _supports_hardlinks(_check_dir)
            if will_copy:
                free_t = _free_space(_check_dir)
                if free_t is not None and free_t < src_size and not cq_state['running']:
                    if not messagebox.askyesno('Low disk space (temp)',
                            'Staging needs a temporary copy of the source on the '
                            'temp drive (%s).\n\nTemp free: %s\nSource image: %s\n\n'
                            'Tip: put the temp folder on the SAME drive as the '
                            'source (and use NTFS) and this copy is skipped '
                            'entirely.\n\nContinue anyway?'
                            % (stage_drv or stage_dir, _fmt_size(free_t),
                               _fmt_size(src_size))):
                        return
        except Exception:
            pass

        state_c['busy'] = True
        btn_c.config(state='disabled')
        prog_c.reset()
        prog_c.update('prep', 0, 'Preparing\u2026')

        def _log(l):
            parent.after(0, lambda x=str(l): app._log('[PFS] ' + x + '\n'))

        do_verify = bool(verify_c_var.get())

        def worker():
            stats = {}
            disk_full = {'hit': False}
            outcome = {'ok': False}  # for queue advance
            pack_src = None        # what 'pack file' consumes
            staged_copy = None     # temp copy of the source image
            staged_dir_rm = None   # mkdtemp staging dir to remove
            src_tmp_dir = None     # .mkpfs_tmp created beside the source
            ascii_link_rm = None   # temp dir holding an ASCII-named link/copy
            def _log_parse(l):
                _log(l)
                _parse_summary(l, stats)
                low = str(l).lower()
                if 'no space left' in low or 'errno 28' in low:
                    disk_full['hit'] = True

            try:
                # Fresh work dir
                try:
                    if os.path.isdir(work_dir):
                        shutil.rmtree(work_dir, ignore_errors=True)
                    os.makedirs(work_dir, exist_ok=True)
                except Exception as e:
                    parent.after(0, lambda e=e: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('Cannot create work folder', error=True),
                        messagebox.showerror('Error', str(e))))
                    return

                _log('Packing the %s as the nested image inside the '
                     '.ffpfsc (no extraction).' % ext)

                # ── Pack into .ffpfsc ─────────────────────────────────
                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    parent.after(0, lambda: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('mkpfs not available', error=True)))
                    return

                try:
                    _cc = int(cpu_cores_c_var.get())
                except Exception:
                    _cc = 1
                _cc = max(1, _cc)

                def _on_progress(phase, pct, total, detail):
                    parent.after(0, lambda ph=phase, p=pct, d=detail:
                        prog_c.update(ph, p, d))

                # The source image itself becomes the nested image inside
                # the compressed .ffpfsc — its filename (and .exfat /
                # .ffpkg extension) is preserved, which is how
                # ShadowMount+/MicroMount recognise it.
                #
                # mkpfs 'pack file' stages the source via a hard link
                # (same volume only), then a symlink (needs privileges on
                # Windows), then errors out — so keep staging on the
                # SOURCE volume when it can hard-link; otherwise copy
                # the image to a capable drive first. Still far cheaper
                # than mounting and extracting.
                step2_tmp = []
                pack_src = src
                src_dir = os.path.dirname(src) or '.'
                if _supports_hardlinks(src_dir):
                    src_tmp_dir = os.path.join(src_dir, '.mkpfs_tmp')
                    try:
                        os.makedirs(src_tmp_dir, exist_ok=True)
                        step2_tmp = ['--temp-folder', src_tmp_dir]
                    except Exception:
                        src_tmp_dir = None
                else:
                    import tempfile as _tf
                    src_sz = os.path.getsize(src)
                    dest_base = None
                    for cand in (work_base, _tf.gettempdir()):
                        try:
                            if cand and os.path.isdir(cand) \
                                    and _supports_hardlinks(cand) \
                                    and (_free_space(cand) or 0) > src_sz:
                                dest_base = cand
                                break
                        except Exception:
                            pass
                    if dest_base:
                        if os.path.normcase(dest_base) == os.path.normcase(work_base):
                            stage_dir = work_dir   # cleaned with work_dir
                        else:
                            stage_dir = _tf.mkdtemp(prefix='pfs_stage_',
                                                    dir=dest_base)
                            staged_dir_rm = stage_dir
                        _log('Source drive cannot hard-link (exFAT/FAT32?) '
                             '\u2014 staging a copy of the image on ' + dest_base)
                        parent.after(0, lambda: prog_c.update('prep', 50,
                            'Staging a copy of the source image\u2026'))
                        staged_copy = os.path.join(stage_dir,
                                                   os.path.basename(src))
                        shutil.copyfile(src, staged_copy)
                        _log('Staged copy: ' + staged_copy)
                        pack_src = staged_copy
                        st_tmp = os.path.join(stage_dir, '.mkpfs_tmp')
                        try:
                            os.makedirs(st_tmp, exist_ok=True)
                            step2_tmp = ['--temp-folder', st_tmp]
                        except Exception:
                            pass
                    else:
                        _log('No hard-link-capable drive with enough free '
                             'space \u2014 letting mkpfs try symlink staging '
                             '(may need admin rights).')

                # mkpfs encodes the nested entry name (the source basename)
                # as strict ASCII (mkpfs/pfs.py to_bytes). A non-ASCII
                # filename such as "PPSA26344 Ghost of Yotei.exfat" (with a
                # macron over the o) crashes the pack with a UnicodeEncodeError.
                # Stage the source under an ASCII-safe name that keeps the same
                # .exfat / .ffpkg extension (which is what ShadowMount+ /
                # MicroMount key on, not the basename), so the nested entry
                # encodes cleanly. A hard link on the same volume is instant
                # and uses no extra space; the output .ffpfsc keeps the user's
                # chosen (possibly non-ASCII) name, which is fine.
                if any(ord(_ch) > 127 for _ch in os.path.basename(pack_src)):
                    import tempfile as _tf3
                    _ext_n  = os.path.splitext(pack_src)[1]
                    _stem_n = os.path.splitext(os.path.basename(pack_src))[0]
                    _ascii_stem = _stem_n.encode('ascii', 'ignore').decode(
                        'ascii').strip().strip('.') or 'image'
                    _safe_name = _ascii_stem + _ext_n
                    try:
                        ascii_link_rm = _tf3.mkdtemp(
                            prefix='pfs_name_',
                            dir=os.path.dirname(pack_src) or None)
                        _safe_path = os.path.join(ascii_link_rm, _safe_name)
                        try:
                            os.link(pack_src, _safe_path)          # instant, same volume
                        except Exception:
                            shutil.copyfile(pack_src, _safe_path)  # cross-volume fallback
                        _log('Source name has non-ASCII characters \u2014 '
                             'packing the nested entry as: ' + _safe_name)
                        pack_src = _safe_path
                    except Exception as _e:
                        _log('Could not stage an ASCII-safe name (%s) \u2014 '
                             'attempting the pack as-is.' % _e)

                # Single pack: source image → compressed .ffpfsc container
                argv2 = ['pack', 'file', '--version', 'PS5',
                         '--inode-bits', '32', '--cpu-count', str(_cc)] \
                        + (['--verify'] if do_verify else []) \
                        + step2_tmp + [pack_src, out_path]
                _log('Pack \u2014 mkpfs ' + ' '.join(argv2))
                parent.after(0, lambda: prog_c.update('prep', 100,
                    'Packing into .ffpfsc\u2026'))
                import time as _t_pack
                _pack_started = _t_pack.time()
                # Live activity poller. mkpfs doesn't report a % while writing a
                # single huge file, so watch the output grow and surface bytes
                # written, rate, elapsed and a clearly-labelled rough ETA \u2014 via
                # set_status only, so it never fights mkpfs's own progress bar.
                _poll_c = {'on': True}
                try:
                    _src_total = os.path.getsize(src)
                except Exception:
                    _src_total = 0
                def _poll_convert():
                    while _poll_c['on']:
                        try:
                            cur = (os.path.getsize(out_path)
                                   if os.path.exists(out_path) else 0)
                            if cur > 0:                      # write has started
                                el = max(0.001, _t_pack.time() - _pack_started)
                                rate = cur / el
                                bits = ['%s written' % _fmt_size(cur),
                                        '%.0f MB/s' % (rate / 1048576),
                                        'elapsed %dm%02ds' % (int(el) // 60, int(el) % 60)]
                                if rate > 0 and _src_total and cur < _src_total:
                                    eta = (_src_total - cur) / rate
                                    bits.append('~%dm%02ds left (est.)'
                                                % (int(eta) // 60, int(eta) % 60))
                                _m = 'Writing \u00b7 ' + '  \u00b7  '.join(bits)
                                parent.after(0, lambda mm=_m: (
                                    prog_c.set_status(mm) if _poll_c['on'] else None))
                        except Exception:
                            pass
                        _t_pack.sleep(2)
                threading.Thread(target=_poll_convert, daemon=True).start()
                rc = run_mkpfs(argv2, log_cb=_log_parse, progress_cb=_on_progress)
                _poll_c['on'] = False

                # mkpfs 'pack file' ALWAYS writes a .ffpfsc (it adjusts the
                # suffix to the container mode). Accept only that exact output,
                # and only when THIS run actually wrote it \u2014 never fall back to a
                # stale .ffpfs/.ffpfsc left by an earlier build, which produced a
                # false "success" that reported the old file.
                actual = out_path
                file_ok = False
                try:
                    file_ok = (os.path.exists(actual)
                               and os.path.getmtime(actual) >= _pack_started - 5)
                except Exception:
                    file_ok = os.path.exists(actual)

                if rc == 0 or (rc == 1 and file_ok):
                    out_sz = os.path.getsize(actual)
                    try:
                        in_sz = os.path.getsize(src)
                    except Exception:
                        in_sz = 0
                    pct = (1 - out_sz / in_sz) * 100 if in_sz else 0.0
                    if in_sz:
                        _log('Compressed: %s \u2192 %s  (%.1f%% smaller)'
                             % (_fmt_size(in_sz), _fmt_size(out_sz), pct))
                    # Optional: delete the source image now the .ffpfsc exists, to
                    # free space (off by default; controlled by a setting). Only
                    # ever after a genuine, freshly-written output.
                    deleted, freed = False, 0
                    try:
                        if app._settings.get('pfs_delete_source_after_convert', False) \
                                and file_ok and os.path.isfile(src) \
                                and os.path.abspath(src) != os.path.abspath(actual):
                            freed = in_sz
                            os.remove(src)
                            deleted = True
                            _log('Deleted source image (freed %s): %s'
                                 % (_fmt_size(freed), src))
                    except Exception as _de:
                        _log('Could not delete source image: %s' % _de)
                    _title = os.path.splitext(os.path.basename(actual))[0]
                    def _done_c(s=dict(stats), _o=out_sz, _i=in_sz, _p=pct,
                                _act=actual, _del=deleted, _fr=freed, _ti=_title):
                        prog_c.done()
                        btn_c.config(state='normal')
                        if _i:
                            prog_c.set_status('Done \u2713  %s  \u00b7  %.1f%% smaller'
                                              % (_fmt_size(_o), _p))
                        else:
                            prog_c.set_status('Done \u2713  %s' % _fmt_size(_o))
                        _show_summary_panel(panel_convert, s, _act)
                        if not cq_state['running']:
                            try:
                                app._trigger_shutdown('pfs')
                            except Exception:
                                pass
                            msg = '\u2705  %s\n\nImage:  %s\n' % (_ti, _act)
                            if _i:
                                msg += 'Size:   %s \u2192 %s   (%.1f%% smaller)\n' % (
                                    _fmt_size(_i), _fmt_size(_o), _p)
                            else:
                                msg += 'Size:   %s\n' % _fmt_size(_o)
                            if _del:
                                msg += 'Source image deleted \u2014 freed %s\n' % _fmt_size(_fr)
                            messagebox.showinfo('Convert complete', msg)
                    outcome['ok'] = True
                    parent.after(0, _done_c)
                elif disk_full['hit']:
                    parent.after(0, lambda: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('Out of disk space', error=True),
                        (messagebox.showerror('Out of disk space',
                            'Ran out of space during conversion. Free space or pick '
                            'another work/output drive.')
                         if not cq_state['running'] else None)))
                else:
                    parent.after(0, lambda: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('Failed (rc=%d) \u2014 see OUTPUT LOG' % rc, error=True),
                        (messagebox.showerror('Failed',
                            'mkpfs exited with code %d.\n\nCheck OUTPUT LOG.' % rc)
                         if not cq_state['running'] else None)))
                    try:
                        if os.path.exists(out_path):
                            os.remove(out_path)
                    except Exception:
                        pass
            except Exception as e:
                _log('Convert error: ' + str(e))
                parent.after(0, lambda e=e: (
                    prog_c.fail(), btn_c.config(state='normal'),
                    prog_c.set_status('Error: ' + str(e)[:60], error=True),
                    (messagebox.showerror('Convert error', str(e))
                     if not cq_state['running'] else None)))
            finally:
                state_c['busy'] = False
                if cq_state['running']:
                    parent.after(0, lambda ok=outcome['ok']: _cq_advance(ok))
                # Clean up staging leftovers
                try:
                    if os.path.isdir(work_dir):
                        shutil.rmtree(work_dir, ignore_errors=True)
                except Exception:
                    pass
                try:
                    if staged_dir_rm and os.path.isdir(staged_dir_rm):
                        shutil.rmtree(staged_dir_rm, ignore_errors=True)
                except Exception:
                    pass
                try:
                    if src_tmp_dir and os.path.isdir(src_tmp_dir):
                        shutil.rmtree(src_tmp_dir, ignore_errors=True)
                except Exception:
                    pass
                try:
                    if ascii_link_rm and os.path.isdir(ascii_link_rm):
                        shutil.rmtree(ascii_link_rm, ignore_errors=True)
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()

    # ════════════════════════════════════════════════════════════════
    # EXTRACT — .ffpfs / .ffpfsc → folder
    # ════════════════════════════════════════════════════════════════
    # ─────────────────────────────────────────────────────────────────
    # Convert queue — batch .exfat/.ffpkg → .ffpfsc, run sequentially
    # ─────────────────────────────────────────────────────────────────
    cq_card = tk.Frame(panel_convert, bg=COLORS['bg_3'],
                       highlightthickness=1, highlightbackground=COLORS['border_3'])
    cq_head = tk.Frame(cq_card, bg=COLORS['bg_3'])
    cq_head.pack(fill='x', padx=16, pady=(12, 6))
    tk.Label(cq_head, text='CONVERT QUEUE', font=FONTS['eyebrow'],
             bg=COLORS['bg_3'], fg=COLORS['fg_5']).pack(side='left')
    cq_count_var = tk.StringVar(value='')
    tk.Label(cq_head, textvariable=cq_count_var, font=FONTS['meta'],
             bg=COLORS['bg_3'], fg=COLORS['fg_4']).pack(side='left', padx=(10, 0))

    cq_list = tk.Frame(cq_card, bg=COLORS['bg_3'])
    cq_list.pack(fill='x', padx=6, pady=(0, 6))

    cq_btns = tk.Frame(cq_card, bg=COLORS['bg_3'])
    cq_btns.pack(fill='x', padx=16, pady=(0, 12))
    make_themed_button(cq_btns, text='Remove selected',
                       command=lambda: _cq_remove_selected(),
                       kind='accent', icon='\u2715', font_size=10,
                       padx=14, pady=8).pack(side='left')
    make_themed_button(cq_btns, text='Clear',
                       command=lambda: _cq_clear(),
                       kind='accent', icon='\u2717', font_size=10,
                       padx=14, pady=8).pack(side='left', padx=(8, 0))

    def _cq_render():
        for w in cq_list.winfo_children():
            w.destroy()
        if not convert_queue:
            cq_card.pack_forget()
            cq_count_var.set('')
            cq_state['sel'].clear()
            return
        cq_card.pack(fill='x', padx=24, pady=(0, 14))
        done   = sum(1 for j in convert_queue if j['status'] == 'done')
        failed = sum(1 for j in convert_queue if j['status'] == 'failed')
        lab = ('%d job%s  \u00b7  %d done'
               % (len(convert_queue), '' if len(convert_queue) == 1 else 's', done))
        if failed:
            lab += '  \u00b7  %d failed' % failed
        cq_count_var.set(lab)
        present = {id(j) for j in convert_queue}
        for k in [k for k in cq_state['sel'] if k not in present]:
            del cq_state['sel'][k]
        running = cq_state['running']
        n = len(convert_queue)
        for idx, job in enumerate(convert_queue):
            editable = (job['status'] == 'queued' and not running)
            row = tk.Frame(cq_list, bg=COLORS['bg_3'])
            row.pack(fill='x', padx=10, pady=2)
            if editable:
                var = cq_state['sel'].get(id(job))
                if var is None:
                    var = tk.BooleanVar(value=False)
                    cq_state['sel'][id(job)] = var
                tk.Checkbutton(row, variable=var, bg=COLORS['bg_3'],
                               activebackground=COLORS['bg_3'],
                               selectcolor=COLORS['bg_4'], highlightthickness=0,
                               bd=0, cursor='hand2').pack(side='left')
            else:
                tk.Frame(row, bg=COLORS['bg_3'], width=20, height=1).pack(side='left')
            tk.Label(row, text=_STATUS_DOT.get(job['status'], '\u25cb'),
                     font=(FONTS['mono_sm'][0], 9), bg=COLORS['bg_3'],
                     fg=_STATUS_COLORS.get(job['status'], COLORS['fg_4'])).pack(
                         side='left', padx=(2, 8))
            tk.Label(row, text=job['name'], font=FONTS['mono_sm'],
                     bg=COLORS['bg_3'], fg=COLORS['fg_1'], anchor='w').pack(
                         side='left', fill='x', expand=True)
            tk.Label(row, text=job['status'], font=FONTS['meta'], bg=COLORS['bg_3'],
                     fg=_STATUS_COLORS.get(job['status'], COLORS['fg_4'])).pack(side='right')
            if editable:
                x = tk.Label(row, text='\u2715', font=FONTS['meta'],
                             bg=COLORS['bg_3'], fg=COLORS['fg_5'], cursor='hand2')
                x.pack(side='right', padx=(0, 10))
                x.bind('<Button-1>', lambda e, j=job: _cq_remove_job(j))
                if idx < n - 1:
                    dn = tk.Label(row, text='\u25bc', font=FONTS['meta'],
                                  bg=COLORS['bg_3'], fg=COLORS['fg_4'], cursor='hand2')
                    dn.pack(side='right', padx=(0, 6))
                    dn.bind('<Button-1>', lambda e, i=idx: _cq_move(i, +1))
                if idx > 0:
                    up = tk.Label(row, text='\u25b2', font=FONTS['meta'],
                                  bg=COLORS['bg_3'], fg=COLORS['fg_4'], cursor='hand2')
                    up.pack(side='right', padx=(0, 6))
                    up.bind('<Button-1>', lambda e, i=idx: _cq_move(i, -1))

    def _cq_add():
        src    = src_c_var.get().strip()
        outdir = outdir_c_var.get().strip()
        name   = name_c_var.get().strip()
        if not src or not os.path.isfile(src):
            messagebox.showerror('Source missing', 'Pick a valid .exfat or .ffpkg image.')
            return
        if os.path.splitext(src)[1].lower() not in ('.exfat', '.ffpkg'):
            messagebox.showerror('Unsupported', 'Source must be a .exfat or .ffpkg image.')
            return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Output folder missing', 'Pick an output folder.')
            return
        if not name:
            name = os.path.splitext(os.path.basename(src))[0]
        if name.lower().endswith('.ffpfs'):
            name = name[:-6] + '.ffpfsc'
        elif not name.lower().endswith('.ffpfsc'):
            name += '.ffpfsc'
        convert_queue.append({
            'src': src, 'outdir': outdir, 'name': name,
            'temp': tempdir_c_var.get().strip(),
            'verify': bool(verify_c_var.get()),
            'status': 'queued',
        })
        _cq_render()

    def _cq_remove_job(job):
        if cq_state['running']:
            return
        try:
            convert_queue.remove(job)
        except ValueError:
            pass
        cq_state['sel'].pop(id(job), None)
        _cq_render()

    def _cq_move(i, delta):
        if cq_state['running']:
            return
        j = i + delta
        if 0 <= j < len(convert_queue):
            convert_queue[i], convert_queue[j] = convert_queue[j], convert_queue[i]
            _cq_render()

    def _cq_remove_selected():
        if cq_state['running']:
            return
        victims = [j for j in convert_queue
                   if id(j) in cq_state['sel'] and cq_state['sel'][id(j)].get()]
        for j in victims:
            try:
                convert_queue.remove(j)
            except ValueError:
                pass
            cq_state['sel'].pop(id(j), None)
        _cq_render()

    def _cq_clear():
        if cq_state['running']:
            return
        convert_queue.clear()
        cq_state['sel'].clear()
        _cq_render()

    def _cq_load_job(i):
        job = convert_queue[i]
        tempdir_c_var.set(job.get('temp', ''))
        verify_c_var.set(job.get('verify', False))
        outdir_c_var.set(job['outdir'])
        name_c_var.set(job['name'])
        src_c_var.set(job['src'])   # set last so its trace fills info/space

    def _cq_start_idx(i):
        cq_state['idx'] = i
        if i >= len(convert_queue):
            cq_state['running'] = False
            btn_c.config(state='normal')
            btn_cq_add.config(state='normal')
            btn_cq_run.config(state='normal')
            _cq_render()
            done   = sum(1 for j in convert_queue if j['status'] == 'done')
            failed = sum(1 for j in convert_queue if j['status'] == 'failed')
            messagebox.showinfo('Convert queue',
                'Queue finished.\n\n%d converted, %d failed.' % (done, failed))
            try:
                app._trigger_shutdown('pfs')
            except Exception:
                pass
            return
        convert_queue[i]['status'] = 'running'
        _cq_render()
        _cq_load_job(i)
        parent.after(80, _run_convert)   # let the var traces settle, then run

    def _cq_advance(ok):
        i = cq_state['idx']
        if 0 <= i < len(convert_queue):
            convert_queue[i]['status'] = 'done' if ok else 'failed'
        _cq_render()
        _cq_start_idx(i + 1)

    def _cq_run_all():
        if cq_state['running'] or state_c['busy']:
            return
        if not any(j['status'] in ('queued', 'failed') for j in convert_queue):
            messagebox.showinfo('Convert queue', 'Nothing queued to convert.')
            return
        for j in convert_queue:
            if j['status'] == 'failed':
                j['status'] = 'queued'
        cq_state['running'] = True
        btn_c.config(state='disabled')
        btn_cq_add.config(state='disabled')
        btn_cq_run.config(state='disabled')
        _cq_render()
        _cq_start_idx(0)

    src_e_var    = tk.StringVar()
    outdir_e_var = tk.StringVar()
    info_e_var   = tk.StringVar(value='')
    state_e      = {'busy': False}

    card_e = tk.Frame(panel_extract, bg=COLORS['bg_2'],
                      highlightthickness=1, highlightbackground=COLORS['border_2'])
    card_e.pack(fill='x', padx=24, pady=(0, 14))
    _card_head(card_e, '\U0001f4e4', 'Extract .ffpfs / .ffpfsc  \u2192  folder',
               'Unpack a PFS image so you can edit files, then rebuild from the Build tab')
    body_e = tk.Frame(card_e, bg=COLORS['bg_2'])
    body_e.pack(fill='x', padx=24, pady=(4, 18))

    note_e = tk.Frame(body_e, bg=COLORS['bg_3'],
                      highlightthickness=1, highlightbackground=COLORS['border_3'])
    note_e.pack(fill='x', pady=(0, 12))
    tk.Label(note_e,
             text=('\u2139  PFS images are read-only \u2014 files cannot be changed in place.\n'
                   'A .ffpfs (uncompressed) unpacks straight to the game files \u2014 best '
                   'for the edit \u2192 rebuild loop.\n'
                   'A .ffpfsc (container) unpacks to its nested image instead '
                   '(pfs_image.dat, or the inner .exfat / .ffpkg) \u2014 extract or mount '
                   'that to reach the game files.'),
             font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_3'],
             anchor='w', justify='left').pack(fill='x', padx=12, pady=8)

    def _browse_src_e():
        p = filedialog.askopenfilename(title='Select PFS image',
            filetypes=[('PFS images', '*.ffpfs *.ffpfsc'), ('All files', '*.*')])
        if p:
            src_e_var.set(p)
    def _browse_out_e():
        p = filedialog.askdirectory(title='Select destination folder')
        if p:
            outdir_e_var.set(p)

    field_block(body_e, 'PFS image (.ffpfs / .ffpfsc)', var=src_e_var,
                on_browse=_browse_src_e, hint='the image to extract')
    field_block(body_e, 'Extract to folder', var=outdir_e_var,
                on_browse=_browse_out_e, hint='a subfolder is created from the image name')

    def _on_src_e(*_):
        p = src_e_var.get().strip()
        if not p or not os.path.isfile(p):
            return
        if not outdir_e_var.get():
            outdir_e_var.set(os.path.dirname(p))
        try:
            info_e_var.set('Image: %s' % _fmt_size(os.path.getsize(p)))
        except Exception:
            info_e_var.set('')
    src_e_var.trace_add('write', _on_src_e)

    tk.Label(body_e, textvariable=info_e_var, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w').pack(fill='x', pady=(4, 8))

    prog_e = _ProgressBlock(body_e, _STAGES_EXTRACT)

    btn_e = make_themed_button(body_e, text='Extract to folder',
                                command=lambda: _run_extract(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=18, pady=9)
    btn_e.pack(anchor='w', pady=(14, 0))

    def _run_extract():
        if state_e['busy']:
            return
        src    = src_e_var.get().strip()
        outdir = outdir_e_var.get().strip()
        if not src or not os.path.isfile(src):
            messagebox.showerror('Source missing', 'Pick a valid .ffpfs or .ffpfsc image.')
            return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Folder missing', 'Pick a destination folder.')
            return
        stem = os.path.splitext(os.path.basename(src))[0]
        dest = os.path.normpath(os.path.join(outdir, stem))
        overwrite = False
        if os.path.exists(dest):
            if not messagebox.askyesno('Folder exists',
                    dest + '\n\nalready exists. Overwrite its contents?'):
                return
            overwrite = True

        state_e['busy'] = True
        btn_e.config(state='disabled')
        prog_e.reset()
        prog_e.update('extract', 0, 'Starting extraction\u2026')

        def _log(l):
            parent.after(0, lambda x=str(l): app._log('[PFS] ' + x + '\n'))

        def worker():
            disk_full = {'hit': False}
            def _log_dl(l):
                _log(l)
                low = str(l).lower()
                if 'no space left' in low or 'errno 28' in low:
                    disk_full['hit'] = True
            try:
                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    parent.after(0, lambda: (
                        prog_e.fail(), btn_e.config(state='normal'),
                        prog_e.set_status('mkpfs not available', error=True),
                        messagebox.showerror('mkpfs not available',
                            'mkpfs is not bundled in this build.')))
                    return
                argv = ['unpack', src, dest]
                if overwrite:
                    argv.append('--overwrite')
                _log('mkpfs ' + ' '.join(argv))

                def _on_progress(phase, pct, total, detail):
                    parent.after(0, lambda p=pct, d=detail:
                        prog_e.update('extract', p, d))

                rc = run_mkpfs(argv, log_cb=_log_dl, progress_cb=_on_progress)

                if rc == 0 and os.path.isdir(dest):
                    n = _count_files(dest)
                    sz = _get_folder_size(dest)
                    parent.after(0, lambda: (
                        prog_e.done(), btn_e.config(state='normal'),
                        prog_e.set_status('Extracted %d files  \u00b7  %s' % (n, _fmt_size(sz))),
                        messagebox.showinfo('Extraction complete',
                            'Extracted to:\n' + dest +
                            '\n\n%d files  \u00b7  %s\n\n'
                            'Edit the files, then rebuild from the Build tab.' % (n, _fmt_size(sz)))))
                elif disk_full['hit']:
                    parent.after(0, lambda: (
                        prog_e.fail(), btn_e.config(state='normal'),
                        prog_e.set_status('Out of disk space', error=True),
                        messagebox.showerror('Out of disk space',
                            'The drive ran out of space during extraction.')))
                else:
                    parent.after(0, lambda: (
                        prog_e.fail(), btn_e.config(state='normal'),
                        prog_e.set_status('Extraction failed (rc=%d)' % rc, error=True),
                        messagebox.showerror('Extraction failed',
                            'mkpfs exited with code %d.\n\nCheck OUTPUT LOG.' % rc)))
            except Exception as e:
                _log('Extract error: ' + str(e))
                parent.after(0, lambda e=e: (
                    prog_e.fail(), btn_e.config(state='normal'),
                    prog_e.set_status('Error: ' + str(e)[:60], error=True),
                    messagebox.showerror('Extract error', str(e))))
            finally:
                state_e['busy'] = False

        threading.Thread(target=worker, daemon=True).start()

    # ── Initial panel display ─────────────────────────────────────────
    _refresh_panels()


def _card_head(card, icon, title, subtitle):
    chead = tk.Frame(card, bg=COLORS['bg_2'])
    chead.pack(fill='x', padx=24, pady=(18, 14))
    ico = tk.Label(chead, text=icon,
                   font=(FONTS['h2'][0], 13),
                   bg=COLORS['accent_08'], fg=COLORS['teal'],
                   width=2, padx=4, pady=2)
    ico.pack(side='left', padx=(0, 12))
    col = tk.Frame(chead, bg=COLORS['bg_2'])
    col.pack(side='left', fill='x', expand=True)
    tk.Label(col, text=title,
             font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(fill='x')
    tk.Label(col, text=subtitle,
             font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w').pack(fill='x', pady=(2, 0))
    tk.Frame(card, bg=COLORS['border_2'], height=1).pack(fill='x')


def _make_cb(parent, text, var):
    def _style(*_):
        cb.config(fg=COLORS['teal'] if var.get() else COLORS['fg_4'])
    cb = tk.Checkbutton(parent, text=text, variable=var,
                         font=FONTS['mono_sm'],
                         bg=COLORS['bg_2'], fg=COLORS['teal'],
                         selectcolor=COLORS['bg_3'],
                         activebackground=COLORS['bg_2'],
                         activeforeground=COLORS['teal'],
                         bd=0, padx=8, cursor='hand2')
    var.trace_add('write', _style)
    _style()
    return cb


_SUMMARY_PATTERNS = {
    'total_files':  r'Total files:\s+(\S+)',
    'size_raw':     r'Total uncompressed size:\s+(.+)',
    'size_stored':  r'Total stored size:\s+(.+)',
    'gain_pct':     r'Actual gain achieved:\s+([\d.]+)%',
    'compressed':   r'Compressed files:\s+(\d+)',
    'uncompressed': r'Uncompressed files:\s+(\d+)',
    'elapsed':      r'Elapsed time:\s+([\d.]+)s',
    'throughput':   r'Throughput:\s+(.+)',
    'version':      r'^Version:\s+(.+)',
    'crc32':        r'Data CRC32:\s+(\S+)',
    'warnings':     r'^Warnings:\s+(\d+)',
    'errors':       r'^Errors:\s+(\d+)',
}


def _parse_summary(line, stats):
    raw = str(line).strip()
    for key, pat in _SUMMARY_PATTERNS.items():
        if key not in stats:
            m = re.search(pat, raw)
            if m:
                stats[key] = m.group(1).strip()


def _show_summary_panel(parent_frame, stats, out_path):
    # Remove any existing summary card
    for w in parent_frame.winfo_children():
        if getattr(w, '_is_summary_card', False):
            w.destroy()

    card = tk.Frame(parent_frame, bg=COLORS['bg_3'],
                    highlightthickness=1,
                    highlightbackground=COLORS['border_3'])
    card._is_summary_card = True
    card.pack(fill='x', padx=24, pady=(12, 0))

    if 'errors' in stats or 'warnings' in stats:
        errs  = int(stats.get('errors',   0) or 0)
        warns = int(stats.get('warnings', 0) or 0)
        badge = ('\u2713  all checks passed' if errs == 0 and warns == 0
                 else '\u26a0  %d verify error%s' % (errs, 's' if errs != 1 else ''))
        badge_fg = COLORS['teal'] if errs == 0 and warns == 0 else COLORS['warn']
    else:
        badge, badge_fg = '\u2713  created', COLORS['teal']

    hdr = tk.Frame(card, bg=COLORS['bg_2'])
    hdr.pack(fill='x')
    tk.Label(hdr, text='BUILD SUMMARY', font=FONTS['eyebrow'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w').pack(
                 side='left', padx=14, pady=8)
    tk.Label(hdr, text=badge, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=badge_fg).pack(side='right', padx=14)
    tk.Frame(card, bg=COLORS['border_3'], height=1).pack(fill='x')

    grid = tk.Frame(card, bg=COLORS['bg_3'])
    grid.pack(fill='x', pady=(6, 10))
    grid.columnconfigure(1, weight=1)

    def row(r, lbl, val, vfg=None):
        tk.Label(grid, text=lbl, font=FONTS['mono_sm'],
                 bg=COLORS['bg_3'], fg=COLORS['fg_4'],
                 anchor='w').grid(row=r, column=0, sticky='w', padx=(14, 8), pady=2)
        tk.Label(grid, text=val,
                 font=(FONTS['mono_sm'][0], FONTS['mono_sm'][1], 'bold'),
                 bg=COLORS['bg_3'], fg=vfg or COLORS['fg_1'],
                 anchor='w').grid(row=r, column=1, sticky='w', padx=(0, 14), pady=2)

    r = 0
    if stats.get('total_files'):
        row(r, 'Files', stats['total_files']); r += 1
    if stats.get('size_raw'):
        row(r, 'Source size', stats['size_raw'].strip()); r += 1
    if stats.get('size_stored'):
        row(r, 'Image size', stats['size_stored'].strip(),
            vfg=COLORS['teal']); r += 1
    if stats.get('gain_pct'):
        gfg = COLORS['teal'] if float(stats['gain_pct']) > 0 else COLORS['fg_3']
        row(r, 'Space saved', stats['gain_pct'] + '%', vfg=gfg); r += 1
    if stats.get('compressed') and stats.get('uncompressed'):
        row(r, 'Compressed',
            '%s files  /  %s raw' % (stats['compressed'], stats['uncompressed'])); r += 1
    if stats.get('elapsed'):
        t = float(stats['elapsed'])
        ts = '%dm %ds' % (int(t) // 60, int(t) % 60) if t >= 60 else '%.1fs' % t
        row(r, 'Elapsed', ts); r += 1
    if stats.get('throughput'):
        row(r, 'Throughput', stats['throughput'].strip()); r += 1
    if stats.get('version'):
        row(r, 'PFS version', stats['version'].strip()); r += 1
    if stats.get('crc32'):
        row(r, 'CRC32', stats['crc32'], vfg=COLORS['fg_3']); r += 1
    if out_path:
        row(r, 'Output', os.path.basename(out_path)); r += 1


# ════════════════════════════════════════════════════════════════════
# Progress block widget
# ════════════════════════════════════════════════════════════════════
class _ProgressBlock:
    """Reusable detailed progress block: big %, stage tiles, bar, detail."""

    def __init__(self, parent, stages):
        self._stages    = stages          # list of (key, label, start, end)
        self._phase     = None
        self._frame     = tk.Frame(parent, bg=COLORS['bg_2'])
        self._frame.pack(fill='x', pady=(18, 0))
        self._build()

    def _build(self):
        f = self._frame

        # Top row: big % + stage name + detail
        top = tk.Frame(f, bg=COLORS['bg_2'])
        top.pack(fill='x', pady=(0, 6))

        self._pct_lbl = tk.Label(top, text='',
                                  font=(FONTS['h2'][0], 28, 'bold'),
                                  bg=COLORS['bg_2'], fg=COLORS['teal'],
                                  width=5, anchor='e')
        self._pct_lbl.pack(side='left')

        right = tk.Frame(top, bg=COLORS['bg_2'])
        right.pack(side='left', padx=(10, 0))
        self._stage_lbl = tk.Label(right, text='',
                                    font=(FONTS['h3'][0], 11, 'bold'),
                                    bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                                    anchor='w')
        self._stage_lbl.pack(anchor='w')
        self._detail_var = tk.StringVar(value='')
        self._detail_lbl = tk.Label(right, textvariable=self._detail_var,
                                     font=FONTS['mono_sm'],
                                     bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                                     anchor='w')
        self._detail_lbl.pack(anchor='w')

        # Progress bar
        self._pbar = ttk.Progressbar(f,
                                      style='Success.Horizontal.TProgressbar',
                                      mode='determinate', maximum=100)
        self._pbar.pack(fill='x', pady=(0, 8))

        # Stage tiles
        tiles_row = tk.Frame(f, bg=COLORS['bg_2'])
        tiles_row.pack(fill='x')
        self._tiles = {}
        for key, label, _s, _e in self._stages:
            tile = tk.Frame(tiles_row, bg=COLORS['bg_3'],
                            highlightthickness=1,
                            highlightbackground=COLORS['border_3'])
            tile.pack(side='left', padx=(0, 6), ipadx=10, ipady=4)
            dot = tk.Label(tile, text='\u25cf',
                           bg=COLORS['bg_3'], fg=COLORS['fg_5'],
                           font=(FONTS['mono_sm'][0], 8))
            dot.pack(side='left', padx=(6, 2))
            lbl = tk.Label(tile, text=label,
                           bg=COLORS['bg_3'], fg=COLORS['fg_4'],
                           font=FONTS['mono_sm'])
            lbl.pack(side='left', padx=(0, 6))
            self._tiles[key] = {'tile': tile, 'dot': dot, 'lbl': lbl}

    def reset(self):
        self._phase = None
        self._pbar['value'] = 0
        self._pct_lbl.config(text='0%', fg=COLORS['teal'])
        self._stage_lbl.config(text='', fg=COLORS['fg_1'])
        self._detail_var.set('')
        for key, w in self._tiles.items():
            w['tile'].config(bg=COLORS['bg_3'],
                             highlightbackground=COLORS['border_3'])
            w['lbl'].config(bg=COLORS['bg_3'], fg=COLORS['fg_4'])
            w['dot'].config(bg=COLORS['bg_3'], fg=COLORS['fg_5'],
                            text='\u25cf')

    def update(self, phase, pct, detail=''):
        if self._phase != phase:
            # Mark previous done
            if self._phase and self._phase in self._tiles:
                w = self._tiles[self._phase]
                w['tile'].config(bg=COLORS['bg_3'],
                                 highlightbackground=COLORS['border_3'])
                w['lbl'].config(bg=COLORS['bg_3'], fg=COLORS['teal'])
                w['dot'].config(bg=COLORS['bg_3'], fg=COLORS['teal'],
                                text='\u2713')
            self._phase = phase
            # Activate new
            if phase in self._tiles:
                w = self._tiles[phase]
                w['tile'].config(bg=COLORS['bg_2'],
                                 highlightbackground=COLORS['teal'])
                w['lbl'].config(bg=COLORS['bg_2'], fg=COLORS['teal'])
                w['dot'].config(bg=COLORS['bg_2'], fg=COLORS['teal'],
                                text='\u25cf')
            label = next((l for k, l, _s, _e in self._stages if k == phase), phase.title())
            self._stage_lbl.config(text=label + '\u2026', fg=COLORS['teal'])

        # Weighted overall %
        entry = next(((s, e) for k, _l, s, e in self._stages if k == phase), (0, 100))
        overall = int(entry[0] + (pct / 100.0) * (entry[1] - entry[0]))
        overall = max(0, min(100, overall))
        self._pbar['value'] = overall
        self._pct_lbl.config(text='%d%%' % overall)

        # Clean detail text
        clean = str(detail).replace(phase, '').strip().lstrip('@').strip()
        self._detail_var.set(clean if clean else detail)

    def set_status(self, msg, error=False):
        self._detail_var.set(msg)
        if error:
            self._stage_lbl.config(text='Failed', fg=COLORS['danger'])
            self._pct_lbl.config(fg=COLORS['danger'])

    def set_detail(self, msg):
        """Update just the detail line (used by the live file-size poller)."""
        self._detail_var.set(msg)

    def done(self):
        for key, w in self._tiles.items():
            w['tile'].config(bg=COLORS['bg_3'],
                             highlightbackground=COLORS['border_3'])
            w['lbl'].config(bg=COLORS['bg_3'], fg=COLORS['teal'])
            w['dot'].config(bg=COLORS['bg_3'], fg=COLORS['teal'],
                            text='\u2713')
        self._pbar['value'] = 100
        self._pct_lbl.config(text='100%', fg=COLORS['teal'])
        self._stage_lbl.config(text='Done \u2713', fg=COLORS['teal'])

    def fail(self):
        if self._phase and self._phase in self._tiles:
            w = self._tiles[self._phase]
            w['tile'].config(highlightbackground=COLORS['danger'])
            w['lbl'].config(fg=COLORS['danger'])
            w['dot'].config(fg=COLORS['danger'], text='\u2717')
        self._pct_lbl.config(fg=COLORS['danger'])
        self._stage_lbl.config(text='Failed', fg=COLORS['danger'])
