"""ui/tab_pfs.py — Build PFS Image tab (two-path workflow)

PFS only works reliably when built from an existing exFAT or ffpkg image.
Building directly from a dump folder produces an image that ShadowMount+
cannot mount (confirmed by the ShadowMount+ author).

This tab offers two paths:
  Path A — Convert existing: user already has .exfat or .ffpkg
  Path B — Auto-build: pick a dump folder, build exfat/ffpkg first,
            then auto-convert to .ffpfs in one pipeline

Both paths show a pre-flight size estimate before starting, and a
detailed per-step progress display during the build:
  Scan (folder size + file count) → exFAT build → PFS compress → PFS write
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
    ('scan',     'Scan',       0,   5),
    ('compress', 'Compress',   5,  92),
    ('write',    'Write',     92, 100),
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
                     'Pack a game dump into a compressed .ffpfs image for ShadowMount+ / MicroMount.')
    head.pack(fill='x', padx=24, pady=(14, 8))

    info_banner(inner,
        '\u2139  PFS images are built from game files (sce_sys/param.json at the root).  '
        'Build from a dump folder, or convert an existing .exfat / .ffpkg \u2014 both '
        'produce a mountable image.'
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
    path_var = tk.StringVar(value='build')  # 'build' | 'extract'

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

    tile_build = _make_path_tile(sel_frame, 'build',
        '\U0001f5c2', 'Build from dump folder',
        'Pick a game dump \u2014 packs directly into a mountable .ffpfs')
    tile_convert = _make_path_tile(sel_frame, 'convert',
        '\U0001f504', 'Convert .exfat / .ffpkg',
        'No dump folder? Extract an image then pack it into a mountable .ffpfs')
    tile_extract = _make_path_tile(sel_frame, 'extract',
        '\U0001f4e4', 'Extract a .ffpfs image',
        'Unpack files from a PFS image to a folder \u2014 edit then rebuild')

    def _refresh_tiles():
        for key, tile in (('build', tile_build), ('convert', tile_convert),
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
        if sel == 'build':
            panel_build.pack(fill='x')
        elif sel == 'convert':
            panel_convert.pack(fill='x')
        else:
            panel_extract.pack(fill='x')

    # ════════════════════════════════════════════════════════════════
    # BUILD — dump folder → .ffpfs (pack folder, the proven method)
    # ════════════════════════════════════════════════════════════════
    src_var       = tk.StringVar()
    outdir_var    = tk.StringVar()
    name_var      = tk.StringVar()
    size_var      = tk.StringVar(value='')
    compress_var    = tk.BooleanVar(value=True)
    version_ps5_var = tk.BooleanVar(value=True)
    tempdir_var   = tk.StringVar()   # optional custom temp/spool folder
    cpu_cores_var = tk.StringVar(value='1')  # advanced: compression CPU cores
    state_b       = {'busy': False}

    card_b = tk.Frame(panel_build, bg=COLORS['bg_2'],
                      highlightthickness=1, highlightbackground=COLORS['border_2'])
    card_b.pack(fill='x', padx=24, pady=(0, 14))
    _card_head(card_b, '\U0001f5c2', 'Dump folder  \u2192  .ffpfs',
               'Packs the game files directly so sce_sys/param.json sits at the image root')
    body_b = tk.Frame(card_b, bg=COLORS['bg_2'])
    body_b.pack(fill='x', padx=24, pady=(4, 18))

    def _browse_src_b():
        p = filedialog.askdirectory(title='Select game dump folder')
        if p:
            src_var.set(p)
    def _browse_out_b():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            outdir_var.set(p)

    field_block(body_b, 'Game dump folder', var=src_var, on_browse=_browse_src_b,
                hint='folder containing sce_sys/param.json and eboot.bin')
    field_block(body_b, 'Output folder', var=outdir_var, on_browse=_browse_out_b,
                hint='where the .ffpfs will be written')
    field_block(body_b, 'Output name', var=name_var,
                hint='auto-filled from folder name if blank')

    def _browse_temp_b():
        p = filedialog.askdirectory(title='Select temp / spool folder')
        if p:
            tempdir_var.set(p)
    field_block(body_b, 'Temp folder (optional)', var=tempdir_var,
                on_browse=_browse_temp_b,
                hint='where compression spools \u2014 defaults to the output drive')

    def _on_src_b(*_):
        p = src_var.get().strip()
        if not p or not os.path.isdir(p):
            return
        if not outdir_var.get():
            outdir_var.set(p)
        base = os.path.basename(p.rstrip('/\\'))
        if not name_var.get():
            name_var.set(base + '.ffpfs')
        size_var.set('Checking dump structure\u2026')
        def _calc():
            root = _find_game_root(p)
            sz = _get_folder_size(p)
            cnt = _count_files(p)
            if root is None:
                msg = ('\u26a0  No sce_sys/param.json found \u2014 this may not be a valid '
                       'game dump.  %s \u00b7 %d files') % (_fmt_size(sz), cnt)
            elif os.path.normpath(root) != os.path.normpath(p):
                msg = ('\u2713  Game found in subfolder: %s\n%s \u00b7 %d files \u00b7 '
                       'PFS estimate ~%s\u2013%s') % (
                    os.path.basename(root), _fmt_size(sz), cnt,
                    _fmt_size(sz * 0.55), _fmt_size(sz * 0.95))
            else:
                msg = ('\u2713  Valid game dump  \u00b7  %s \u00b7 %d files\n'
                       'PFS estimate ~%s\u2013%s (after compression)') % (
                    _fmt_size(sz), cnt, _fmt_size(sz * 0.55), _fmt_size(sz * 0.95))
            parent.after(0, lambda: size_var.set(msg))
        threading.Thread(target=_calc, daemon=True).start()
    src_var.trace_add('write', _on_src_b)

    size_lbl = tk.Label(body_b, textvariable=size_var, font=FONTS['mono_sm'],
                        bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w', justify='left')
    size_lbl.pack(fill='x', pady=(8, 4))

    opts_b = tk.Frame(body_b, bg=COLORS['bg_2'])
    opts_b.pack(fill='x', pady=(8, 0))
    tk.Label(opts_b, text='Options:', font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')
    for text, var in (('--compress', compress_var), ('--version PS5', version_ps5_var)):
        cb = _make_cb(opts_b, text, var)
        cb.pack(side='left', padx=(10 if text == '--compress' else 0, 0))
    tk.Label(opts_b, text='  \u2756  recommended for MicroMount',
             font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_5']).pack(side='left')

    # ── Advanced: CPU cores for compression ──────────────────────────
    try:
        import multiprocessing as _mp
        _max_cores = max(1, _mp.cpu_count())
    except Exception:
        _max_cores = 1
    adv_b = tk.Frame(body_b, bg=COLORS['bg_2'])
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

    prog_b = _ProgressBlock(body_b, _STAGES_BUILD)

    btn_b = make_themed_button(body_b, text='Build .ffpfs',
                                command=lambda: _run_build(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=18, pady=9)
    btn_b.pack(anchor='w', pady=(14, 0))

    def _run_build():
        if state_b['busy']:
            return
        src    = src_var.get().strip()
        outdir = outdir_var.get().strip()
        name   = name_var.get().strip()
        if not src or not os.path.isdir(src):
            messagebox.showerror('Source missing', 'Pick a valid game dump folder.')
            return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Output folder missing', 'Pick an output folder.')
            return
        if not name:
            messagebox.showerror('Output name missing', 'Set an output filename.')
            return
        if not name.lower().endswith('.ffpfs'):
            name += '.ffpfs'

        # Find the actual game root (handles one level of nesting)
        game_root = _find_game_root(src)
        if game_root is None:
            if not messagebox.askyesno('No game files found',
                    'sce_sys/param.json was not found in this folder or one level down.\n\n'
                    'The resulting image may not mount on the PS5.\n\nContinue anyway?'):
                return
            game_root = src
        elif os.path.normpath(game_root) != os.path.normpath(src):
            messagebox.showinfo('Game found in subfolder',
                'Game files were found in:\n' + game_root +
                '\n\nThe image will be packed from there so the layout is correct.')

        out_path = os.path.normpath(os.path.join(outdir, name))
        if os.path.exists(out_path):
            if not messagebox.askyesno('Overwrite', out_path + '\n\nalready exists. Overwrite?'):
                return
            try:
                os.remove(out_path)
            except Exception:
                pass

        # Free-space check
        try:
            need = _get_folder_size(game_root)
            free = _free_space(outdir)
            if free is not None and free < need * 0.95:
                if not messagebox.askyesno('Low disk space',
                        'Output drive free: %s\nDump size: %s\n\n'
                        'You may run out of space. Continue anyway?'
                        % (_fmt_size(free), _fmt_size(need))):
                    return
        except Exception:
            pass

        # Warn the first time someone uses more than 1 core
        try:
            if int(cpu_cores_var.get()) > 1 and not state_b.get('warned_cores'):
                if not messagebox.askyesno('Multi-core compression',
                        'Using more than 1 CPU core speeds up compression a lot, '
                        'but uses more RAM and is less battle-tested in the packaged '
                        'app.\n\nIf you see any odd behaviour, drop back to 1 core.\n\n'
                        'Continue with %s cores?' % cpu_cores_var.get()):
                    return
                state_b['warned_cores'] = True
        except Exception:
            pass

        state_b['busy'] = True
        btn_b.config(state='disabled')
        prog_b.reset()
        prog_b.update('scan', 0, 'Starting\u2026')

        def _log(l):
            parent.after(0, lambda x=str(l): app._log('[PFS] ' + x + '\n'))

        def worker():
            stats = {}
            disk_full = {'hit': False}
            _tmp = None
            def _log_parse(l):
                _log(l)
                _parse_summary(l, stats)
                low = str(l).lower()
                if 'no space left' in low or 'errno 28' in low:
                    disk_full['hit'] = True
            try:
                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    parent.after(0, lambda: (
                        prog_b.fail(), btn_b.config(state='normal'),
                        prog_b.set_status('mkpfs not available', error=True),
                        messagebox.showerror('mkpfs not available',
                            'mkpfs is not bundled in this build.')))
                    return

                argv = ['pack', 'folder']
                if compress_var.get():
                    argv.append('--compress')
                if version_ps5_var.get():
                    argv += ['--version', 'PS5']
                # Advanced: CPU cores. Default 1 is safest in the frozen exe;
                # freeze_support() makes >1 workers safe, but warn the user.
                try:
                    _cores = int(cpu_cores_var.get())
                except Exception:
                    _cores = 1
                argv += ['--cpu-count', str(max(1, _cores))]
                # Spool temp artifacts to a folder with room. Use the user's
                # custom temp folder if set, otherwise a .mkpfs_tmp folder on
                # the OUTPUT drive (avoids filling C:/%TEMP% on huge images).
                try:
                    custom_temp = tempdir_var.get().strip()
                    if custom_temp and os.path.isdir(custom_temp):
                        _tmp = os.path.join(custom_temp, '.mkpfs_tmp')
                    else:
                        _tmp = os.path.join(outdir, '.mkpfs_tmp')
                    os.makedirs(_tmp, exist_ok=True)
                    argv += ['--temp-folder', _tmp]
                except Exception:
                    _tmp = None
                argv += [game_root, out_path]
                _log('mkpfs ' + ' '.join(argv))

                def _on_progress(phase, pct, total, detail):
                    parent.after(0, lambda ph=phase, p=pct, d=detail:
                        prog_b.update(ph, p, d))

                rc = run_mkpfs(argv, log_cb=_log_parse, progress_cb=_on_progress)

                actual = out_path
                for _ext in ('.ffpfs', '.ffpfsc'):
                    _cand = os.path.normpath(os.path.splitext(out_path)[0] + _ext)
                    if os.path.exists(_cand):
                        actual = _cand
                        break
                file_ok = os.path.exists(actual)

                if rc == 0 or (rc == 1 and file_ok):
                    sz = os.path.getsize(actual) / 1024**3
                    parent.after(0, lambda s=dict(stats): (
                        prog_b.done(), btn_b.config(state='normal'),
                        prog_b.set_status('Done \u2713  %.2f GB' % sz),
                        _show_summary_panel(panel_build, s, actual),
                        messagebox.showinfo('Done', 'PFS image written:\n' + actual)
                        if rc == 0 else
                        messagebox.showwarning('Done (verify warnings)',
                            'PFS image written:\n' + actual +
                            '\n\nVerify reported differences \u2014 see OUTPUT LOG.')))
                elif disk_full['hit']:
                    parent.after(0, lambda: (
                        prog_b.fail(), btn_b.config(state='normal'),
                        prog_b.set_status('Out of disk space', error=True),
                        messagebox.showerror('Out of disk space',
                            'The output drive ran out of space during the build.\n\n'
                            'Free up space or choose a different output drive.')))
                else:
                    parent.after(0, lambda: (
                        prog_b.fail(), btn_b.config(state='normal'),
                        prog_b.set_status('Failed (rc=%d) \u2014 see OUTPUT LOG' % rc, error=True),
                        messagebox.showerror('Failed',
                            'mkpfs exited with code %d.\n\nCheck OUTPUT LOG.' % rc)))
                    try:
                        if os.path.exists(out_path):
                            os.remove(out_path)
                    except Exception:
                        pass
            except Exception as e:
                _log('Build error: ' + str(e))
                parent.after(0, lambda e=e: (
                    prog_b.fail(), btn_b.config(state='normal'),
                    prog_b.set_status('Error: ' + str(e)[:60], error=True),
                    messagebox.showerror('Build error', str(e))))
            finally:
                state_b['busy'] = False
                # Remove the temp spool folder we created on the output drive
                try:
                    if _tmp and os.path.isdir(_tmp):
                        shutil.rmtree(_tmp, ignore_errors=True)
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()

    # ════════════════════════════════════════════════════════════════
    # CONVERT — existing .exfat / .ffpkg → .ffpfs (extract then pack folder)
    # ════════════════════════════════════════════════════════════════
    src_c_var     = tk.StringVar()
    outdir_c_var  = tk.StringVar()
    name_c_var    = tk.StringVar()
    tempdir_c_var = tk.StringVar()
    info_c_var    = tk.StringVar(value='')
    compress_c_var    = tk.BooleanVar(value=True)
    version_ps5_c_var = tk.BooleanVar(value=True)
    state_c       = {'busy': False}

    card_c = tk.Frame(panel_convert, bg=COLORS['bg_2'],
                      highlightthickness=1, highlightbackground=COLORS['border_2'])
    card_c.pack(fill='x', padx=24, pady=(0, 14))
    _card_head(card_c, '\U0001f504', 'Convert .exfat / .ffpkg  \u2192  .ffpfs',
               'Extracts the image to files, then packs them so the PFS mounts correctly')
    body_c = tk.Frame(card_c, bg=COLORS['bg_2'])
    body_c.pack(fill='x', padx=24, pady=(4, 18))

    note_c = tk.Frame(body_c, bg=COLORS['bg_3'],
                      highlightthickness=1, highlightbackground=COLORS['border_3'])
    note_c.pack(fill='x', pady=(0, 12))
    tk.Label(note_c,
             text=('\u2139  This extracts the image first (needs free space for the '
                   'extracted game), then packs the files into a mountable .ffpfs.\n'
                   'exFAT needs OSFMount; ffpkg uses the bundled UFS2Tool.'),
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
                on_browse=_browse_src_c, hint='the image to convert')
    field_block(body_c, 'Output folder', var=outdir_c_var,
                on_browse=_browse_out_c, hint='where the .ffpfs will be written')
    field_block(body_c, 'Output name', var=name_c_var,
                hint='auto-filled from source name if blank')
    field_block(body_c, 'Temp / work folder (optional)', var=tempdir_c_var,
                on_browse=_browse_temp_c,
                hint='where the image is extracted \u2014 needs room for the full game')

    def _on_src_c(*_):
        p = src_c_var.get().strip()
        if not p or not os.path.isfile(p):
            return
        if not outdir_c_var.get():
            outdir_c_var.set(os.path.dirname(p))
        if not name_c_var.get():
            name_c_var.set(os.path.splitext(os.path.basename(p))[0] + '.ffpfs')
        try:
            sz = os.path.getsize(p)
            ext = os.path.splitext(p)[1].lower()
            tool = 'OSFMount' if ext == '.exfat' else 'UFS2Tool' if ext == '.ffpkg' else '?'
            info_c_var.set('Image: %s  \u00b7  will extract via %s then pack' % (_fmt_size(sz), tool))
        except Exception:
            info_c_var.set('')
    src_c_var.trace_add('write', _on_src_c)

    tk.Label(body_c, textvariable=info_c_var, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w').pack(fill='x', pady=(4, 8))

    opts_c = tk.Frame(body_c, bg=COLORS['bg_2'])
    opts_c.pack(fill='x', pady=(8, 0))
    tk.Label(opts_c, text='Options:', font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')
    for text, var in (('--compress', compress_c_var), ('--version PS5', version_ps5_c_var)):
        cb = _make_cb(opts_c, text, var)
        cb.pack(side='left', padx=(10 if text == '--compress' else 0, 0))

    _STAGES_CONVERT2 = [
        ('extract',  'Extract',   0,  35),
        ('scan',     'Scan',     35,  40),
        ('compress', 'Compress', 40,  92),
        ('write',    'Write',    92, 100),
    ]
    prog_c = _ProgressBlock(body_c, _STAGES_CONVERT2)

    btn_c = make_themed_button(body_c, text='Convert to .ffpfs',
                                command=lambda: _run_convert(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=18, pady=9)
    btn_c.pack(anchor='w', pady=(14, 0))

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
        if not name.lower().endswith('.ffpfs'):
            name += '.ffpfs'
        out_path = os.path.normpath(os.path.join(outdir, name))

        # Work folder for extraction
        work_base = tempdir_c_var.get().strip() or outdir
        work_dir  = os.path.normpath(os.path.join(work_base, '.pfs_convert_work'))

        if os.path.exists(out_path):
            if not messagebox.askyesno('Overwrite', out_path + '\n\nalready exists. Overwrite?'):
                return
            try:
                os.remove(out_path)
            except Exception:
                pass

        # Free-space: need room for extracted game (~source size) + final PFS
        try:
            need = int(os.path.getsize(src) * 1.6)
            free = _free_space(work_base)
            if free is not None and free < need:
                if not messagebox.askyesno('Low disk space',
                        'Converting extracts the full game first.\n\n'
                        'Work drive free: %s\nNeeds roughly: %s\n\nContinue anyway?'
                        % (_fmt_size(free), _fmt_size(need))):
                    return
        except Exception:
            pass

        state_c['busy'] = True
        btn_c.config(state='disabled')
        prog_c.reset()
        prog_c.update('extract', 0, 'Preparing\u2026')

        def _log(l):
            parent.after(0, lambda x=str(l): app._log('[PFS] ' + x + '\n'))

        def worker():
            stats = {}
            disk_full = {'hit': False}
            extracted = None
            def _log_parse(l):
                _log(l)
                _parse_summary(l, stats)
                low = str(l).lower()
                if 'no space left' in low or 'errno 28' in low:
                    disk_full['hit'] = True

            CREATE_NO_WINDOW = 0x08000000
            kw = {'creationflags': CREATE_NO_WINDOW} if os.name == 'nt' else {}

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

                extracted = os.path.join(work_dir, 'extracted')
                os.makedirs(extracted, exist_ok=True)

                # ── Extract ───────────────────────────────────────────
                if ext == '.ffpkg':
                    ufs = getattr(app, '_ffpkg_ufs2_exe', None)
                    if not ufs or not os.path.isfile(ufs):
                        try:
                            from exfat_builder import extract_ufs2tool
                            ufs = extract_ufs2tool()
                            app._ffpkg_ufs2_exe = ufs
                        except Exception:
                            ufs = None
                    if not ufs or not os.path.isfile(ufs):
                        parent.after(0, lambda: (
                            prog_c.fail(), btn_c.config(state='normal'),
                            prog_c.set_status('UFS2Tool not available', error=True),
                            messagebox.showerror('UFS2Tool missing',
                                'Could not locate UFS2Tool for ffpkg extraction.')))
                        return
                    cmd = [ufs, 'extract', src, extracted]
                    _log('Extracting ffpkg: ' + ' '.join('"%s"' % a for a in cmd))
                    parent.after(0, lambda: prog_c.update('extract', 5, 'Extracting ffpkg\u2026'))
                    _enc = 'mbcs' if sys.platform.startswith('win') else 'utf-8'
                    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, encoding=_enc,
                        errors='replace', **kw)
                    pc = [5]
                    for line in proc.stdout:
                        _log(line.rstrip())
                        if any(k in line for k in ('Extracted', 'Writing', 'extracting')):
                            pc[0] = min(34, pc[0] + 1)
                            parent.after(0, lambda p=pc[0]: prog_c.update('extract', p, 'Extracting ffpkg\u2026'))
                    proc.wait()
                    rc_ex = proc.returncode
                else:
                    # exFAT — mount read-only via OSFMount, copy out, dismount
                    osf = None
                    try:
                        osf = app._find_osfmount()
                    except Exception:
                        pass
                    if not osf or not os.path.isfile(osf):
                        parent.after(0, lambda: (
                            prog_c.fail(), btn_c.config(state='normal'),
                            prog_c.set_status('OSFMount not found', error=True),
                            messagebox.showerror('OSFMount missing',
                                'OSFMount is required to extract exFAT images.\n\n'
                                'Set it in Settings \u2192 OSFMount.')))
                        return
                    # Pick a free drive letter
                    import string
                    used = set()
                    try:
                        import ctypes
                        bits = ctypes.windll.kernel32.GetLogicalDrives()
                        used = {string.ascii_uppercase[i] for i in range(26) if bits & (1 << i)}
                    except Exception:
                        pass
                    free_letter = None
                    for L in 'ZYXWVUTSRQPONMLKJIHG':
                        if L not in used:
                            free_letter = L + ':'
                            break
                    if not free_letter:
                        parent.after(0, lambda: (
                            prog_c.fail(), btn_c.config(state='normal'),
                            prog_c.set_status('No free drive letter', error=True)))
                        return
                    _log('Mounting exFAT read-only on ' + free_letter)
                    parent.after(0, lambda: prog_c.update('extract', 5, 'Mounting exFAT\u2026'))
                    m = subprocess.run([osf, '-a', '-t', 'file', '-f', src,
                                        '-m', free_letter, '-o', 'ro,rem'],
                                       capture_output=True, text=True, **kw)
                    if m.returncode != 0:
                        _log('OSFMount mount failed: ' + (m.stdout or '') + (m.stderr or ''))
                        parent.after(0, lambda: (
                            prog_c.fail(), btn_c.config(state='normal'),
                            prog_c.set_status('Mount failed', error=True),
                            messagebox.showerror('Mount failed',
                                'OSFMount could not mount the exFAT image.\nCheck OUTPUT LOG.')))
                        return
                    try:
                        parent.after(0, lambda: prog_c.update('extract', 10, 'Copying files\u2026'))
                        cmd = ['robocopy', free_letter + '\\', extracted,
                               '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/R:2', '/W:2']
                        _log('Copying: ' + ' '.join(cmd))
                        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors='replace', **kw)
                        for line in proc.stdout:
                            s = line.strip()
                            if s and '%' not in s:
                                _log(s)
                        proc.wait()
                        # robocopy rc < 8 = success
                        rc_ex = 0 if proc.returncode < 8 else proc.returncode
                    finally:
                        subprocess.run([osf, '-d', '-m', free_letter],
                                       capture_output=True, text=True, **kw)
                        _log('Dismounted ' + free_letter)

                if rc_ex != 0:
                    parent.after(0, lambda: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('Extraction failed (rc=%d)' % rc_ex, error=True),
                        messagebox.showerror('Extraction failed',
                            'Could not extract the image (rc=%d).\nCheck OUTPUT LOG.' % rc_ex)))
                    return

                # ── Locate game root in extracted files ───────────────
                parent.after(0, lambda: prog_c.update('scan', 0, 'Checking game structure\u2026'))
                game_root = _find_game_root(extracted)
                if game_root is None:
                    parent.after(0, lambda: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('No sce_sys/param.json found', error=True),
                        messagebox.showerror('Invalid game image',
                            'The extracted image has no sce_sys/param.json at its root.\n'
                            'It may not be a valid PS5 game image.')))
                    return
                parent.after(0, lambda: prog_c.update('scan', 100, 'Game found'))

                # ── Pack folder → .ffpfs ──────────────────────────────
                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    parent.after(0, lambda: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('mkpfs not available', error=True)))
                    return

                argv = ['pack', 'folder']
                if compress_c_var.get():
                    argv.append('--compress')
                if version_ps5_c_var.get():
                    argv += ['--version', 'PS5']
                if getattr(sys, 'frozen', False):
                    argv += ['--cpu-count', '1']
                # Spool to the work drive
                _tmp = os.path.join(work_dir, '.mkpfs_tmp')
                try:
                    os.makedirs(_tmp, exist_ok=True)
                    argv += ['--temp-folder', _tmp]
                except Exception:
                    pass
                argv += [game_root, out_path]
                _log('mkpfs ' + ' '.join(argv))

                def _on_progress(phase, pct, total, detail):
                    parent.after(0, lambda ph=phase, p=pct, d=detail:
                        prog_c.update(ph, p, d))

                rc = run_mkpfs(argv, log_cb=_log_parse, progress_cb=_on_progress)

                actual = out_path
                for _e in ('.ffpfs', '.ffpfsc'):
                    cnd = os.path.normpath(os.path.splitext(out_path)[0] + _e)
                    if os.path.exists(cnd):
                        actual = cnd
                        break
                file_ok = os.path.exists(actual)

                if rc == 0 or (rc == 1 and file_ok):
                    sz = os.path.getsize(actual) / 1024**3
                    parent.after(0, lambda s=dict(stats): (
                        prog_c.done(), btn_c.config(state='normal'),
                        prog_c.set_status('Done \u2713  %.2f GB' % sz),
                        _show_summary_panel(panel_convert, s, actual),
                        messagebox.showinfo('Done', 'PFS image written:\n' + actual)))
                elif disk_full['hit']:
                    parent.after(0, lambda: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('Out of disk space', error=True),
                        messagebox.showerror('Out of disk space',
                            'Ran out of space during conversion. Free space or pick '
                            'another work/output drive.')))
                else:
                    parent.after(0, lambda: (
                        prog_c.fail(), btn_c.config(state='normal'),
                        prog_c.set_status('Failed (rc=%d) \u2014 see OUTPUT LOG' % rc, error=True),
                        messagebox.showerror('Failed',
                            'mkpfs exited with code %d.\n\nCheck OUTPUT LOG.' % rc)))
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
                    messagebox.showerror('Convert error', str(e))))
            finally:
                state_c['busy'] = False
                # Clean up the entire work folder (extracted files + spool)
                try:
                    if os.path.isdir(work_dir):
                        shutil.rmtree(work_dir, ignore_errors=True)
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()

    # ════════════════════════════════════════════════════════════════
    # EXTRACT — .ffpfs / .ffpfsc → folder
    # ════════════════════════════════════════════════════════════════
    src_e_var    = tk.StringVar()
    outdir_e_var = tk.StringVar()
    info_e_var   = tk.StringVar(value='')
    state_e      = {'busy': False}

    card_e = tk.Frame(panel_extract, bg=COLORS['bg_2'],
                      highlightthickness=1, highlightbackground=COLORS['border_2'])
    card_e.pack(fill='x', padx=24, pady=(0, 14))
    _card_head(card_e, '\U0001f4e4', 'Extract .ffpfs  \u2192  folder',
               'Unpack a PFS image so you can edit files, then rebuild from the Build tab')
    body_e = tk.Frame(card_e, bg=COLORS['bg_2'])
    body_e.pack(fill='x', padx=24, pady=(4, 18))

    note_e = tk.Frame(body_e, bg=COLORS['bg_3'],
                      highlightthickness=1, highlightbackground=COLORS['border_3'])
    note_e.pack(fill='x', pady=(0, 12))
    tk.Label(note_e,
             text=('\u2139  PFS images are read-only \u2014 files cannot be changed in place.\n'
                   'To edit: extract here \u2192 modify the folder \u2192 rebuild from Build.'),
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

    errs  = int(stats.get('errors',   0) or 0)
    warns = int(stats.get('warnings', 0) or 0)
    badge = ('\u2713  all checks passed' if errs == 0 and warns == 0
             else '\u26a0  %d verify error%s' % (errs, 's' if errs != 1 else ''))
    badge_fg = COLORS['teal'] if errs == 0 and warns == 0 else COLORS['warn']

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
