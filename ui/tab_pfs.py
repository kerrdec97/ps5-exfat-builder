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

# ── Stage weights for overall progress bar ───────────────────────────
# Path A (convert only): just compress + write
_STAGES_CONVERT = [
    ('compress', 'Compress',  5,  90),
    ('write',    'Write',    90, 100),
]
# Path B (dump → exfat → pfs): scan + build exfat + compress + write
_STAGES_FULL = [
    ('scan',     'Scan',       0,   3),
    ('exfat',    'Build exFAT', 3,  40),
    ('compress', 'Compress',  40,  95),
    ('write',    'Write',     95, 100),
]


def _fmt_size(b):
    if b >= 1024**3:
        return '%.2f GB' % (b / 1024**3)
    if b >= 1024**2:
        return '%.0f MB' % (b / 1024**2)
    return '%.0f KB' % (b / 1024)


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
                     'Create a .ffpfs image ready for ShadowMount+ or MicroMount.')
    head.pack(fill='x', padx=24, pady=(14, 8))

    info_banner(inner,
        '\u26a0  PFS only mounts correctly when converted from exFAT or ffpkg.  '
        'Building directly from a dump folder is not supported by ShadowMount+.'
    ).pack(fill='x', padx=24, pady=(0, 12))

    # ── Path selector ────────────────────────────────────────────────
    path_var = tk.StringVar(value='convert')  # 'convert' | 'full' | 'extract'

    sel_frame = tk.Frame(inner, bg=COLORS['bg_1'])
    sel_frame.pack(fill='x', padx=24, pady=(0, 14))

    def _make_path_tile(parent, key, icon, title, subtitle):
        outer = tk.Frame(parent, bg=COLORS['bg_2'],
                         highlightthickness=2,
                         highlightbackground=COLORS['border_2'],
                         cursor='hand2')
        outer.pack(side='left', fill='both', expand=True, padx=(0, 8))

        icon_lbl = tk.Label(outer, text=icon,
                             font=(FONTS['h2'][0], 20),
                             bg=COLORS['bg_2'], fg=COLORS['fg_4'])
        icon_lbl.pack(pady=(18, 4))
        title_lbl = tk.Label(outer, text=title,
                              font=(FONTS['h3'][0], 11, 'bold'),
                              bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                              wraplength=220, justify='center')
        title_lbl.pack(padx=12)
        sub_lbl = tk.Label(outer, text=subtitle,
                            font=FONTS['meta'],
                            bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                            wraplength=220, justify='center')
        sub_lbl.pack(padx=12, pady=(4, 18))

        widgets = {'outer': outer, 'icon': icon_lbl,
                   'title': title_lbl, 'sub': sub_lbl}

        def _select(_=None, k=key):
            path_var.set(k)
            _refresh_path_tiles()
            _refresh_panels()

        for w in (outer, icon_lbl, title_lbl, sub_lbl):
            w.bind('<Button-1>', _select)
        return widgets

    tile_a = _make_path_tile(sel_frame, 'convert',
        '\U0001f504',
        'Convert existing image',
        'I already have a .exfat or .ffpkg \u2014 convert it to .ffpfs')
    tile_b = _make_path_tile(sel_frame, 'full',
        '\U0001f5c2',
        'Build from dump folder',
        'Pick a game dump folder \u2014 build exFAT then auto-convert to .ffpfs')
    tile_c = _make_path_tile(sel_frame, 'extract',
        '\U0001f4e4',
        'Extract a .ffpfs image',
        'Unpack files from a PFS image to a folder \u2014 edit then rebuild')

    def _refresh_path_tiles():
        for key, tile in (('convert', tile_a), ('full', tile_b), ('extract', tile_c)):
            active = path_var.get() == key
            tile['outer'].config(
                highlightbackground=COLORS['teal'] if active else COLORS['border_2'],
                bg=COLORS['bg_2'] if not active else COLORS['bg_3'])
            for w in (tile['icon'], tile['title'], tile['sub']):
                w.config(bg=COLORS['bg_2'] if not active else COLORS['bg_3'],
                         fg=(COLORS['teal'] if active and w is tile['title']
                             else COLORS['fg_1'] if w is tile['title']
                             else COLORS['teal'] if active and w is tile['icon']
                             else COLORS['fg_4']))

    _refresh_path_tiles()

    # ── Panel A: convert existing ─────────────────────────────────────
    panel_a = tk.Frame(inner, bg=COLORS['bg_1'])

    panel_b = tk.Frame(inner, bg=COLORS['bg_1'])

    panel_c = tk.Frame(inner, bg=COLORS['bg_1'])

    def _refresh_panels():
        for p in (panel_a, panel_b, panel_c):
            p.pack_forget()
        if path_var.get() == 'convert':
            panel_a.pack(fill='x')
        elif path_var.get() == 'full':
            panel_b.pack(fill='x')
        else:
            panel_c.pack(fill='x')

    # ════════════════════════════════════════════════════════════════
    # PANEL A — Convert existing .exfat / .ffpkg → .ffpfs
    # ════════════════════════════════════════════════════════════════
    src_a_var    = tk.StringVar()
    outdir_a_var = tk.StringVar()
    name_a_var   = tk.StringVar()
    size_a_var   = tk.StringVar(value='')
    compress_a_var     = tk.BooleanVar(value=True)
    version_ps5_a_var  = tk.BooleanVar(value=True)

    card_a = tk.Frame(panel_a, bg=COLORS['bg_2'],
                      highlightthickness=1,
                      highlightbackground=COLORS['border_2'])
    card_a.pack(fill='x', padx=24, pady=(0, 14))

    _card_head(card_a, '\U0001f504', 'Convert .exfat / .ffpkg  \u2192  .ffpfs',
               'No re-build needed \u2014 mkpfs repacks the existing filesystem into PFS')

    body_a = tk.Frame(card_a, bg=COLORS['bg_2'])
    body_a.pack(fill='x', padx=24, pady=(4, 18))

    def _browse_src_a():
        p = filedialog.askopenfilename(
            title='Select source image',
            filetypes=[('Game images', '*.exfat *.ffpkg'),
                       ('exFAT images', '*.exfat'),
                       ('ffpkg images', '*.ffpkg'),
                       ('All files', '*.*')])
        if p:
            src_a_var.set(p)

    def _browse_out_a():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            outdir_a_var.set(p)

    field_block(body_a, 'Source image (.exfat or .ffpkg)',
                var=src_a_var, on_browse=_browse_src_a,
                hint='the image to convert')
    field_block(body_a, 'Output folder',
                var=outdir_a_var, on_browse=_browse_out_a,
                hint='where the .ffpfs will be written')
    field_block(body_a, 'Output name',
                var=name_a_var,
                hint='auto-filled from source name if blank')

    def _on_src_a(*_):
        p = src_a_var.get()
        if not p:
            return
        if not outdir_a_var.get():
            outdir_a_var.set(os.path.dirname(p))
        if not name_a_var.get():
            name_a_var.set(os.path.splitext(os.path.basename(p))[0] + '.ffpfs')
        # Show file size
        try:
            sz = os.path.getsize(p)
            size_a_var.set('Source: %s \u2014 PFS image will be similar size or smaller' % _fmt_size(sz))
        except Exception:
            size_a_var.set('')
    src_a_var.trace_add('write', _on_src_a)

    # Size estimate row
    size_a_lbl = tk.Label(body_a, textvariable=size_a_var,
                           font=FONTS['mono_sm'],
                           bg=COLORS['bg_2'], fg=COLORS['teal'],
                           anchor='w')
    size_a_lbl.pack(fill='x', pady=(4, 8))

    # Options
    opts_a = tk.Frame(body_a, bg=COLORS['bg_2'])
    opts_a.pack(fill='x', pady=(4, 0))
    tk.Label(opts_a, text='Options:', font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')
    for text, var in (('--compress', compress_a_var), ('--version PS5', version_ps5_a_var)):
        cb = _make_cb(opts_a, text, var)
        cb.pack(side='left', padx=(10 if text == '--compress' else 0, 0))

    # ── Progress + action for panel A ────────────────────────────────
    prog_a = _ProgressBlock(body_a, _STAGES_CONVERT)

    btn_a = make_themed_button(body_a, text='Convert to .ffpfs',
                                command=lambda: _run_convert_a(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=18, pady=9)
    btn_a.pack(anchor='w', pady=(14, 0))

    state_a = {'busy': False}

    def _run_convert_a():
        if state_a['busy']:
            return
        src    = src_a_var.get().strip()
        outdir = outdir_a_var.get().strip()
        name   = name_a_var.get().strip()
        if not src or not os.path.isfile(src):
            messagebox.showerror('Source missing', 'Pick a valid .exfat or .ffpkg file.')
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
        if os.path.exists(out_path):
            if not messagebox.askyesno('Overwrite',
                    out_path + '\n\nalready exists. Overwrite?'):
                return
            try:
                os.remove(out_path)
            except Exception as e:
                return

        state_a['busy'] = True
        btn_a.config(state='disabled')
        prog_a.reset()
        prog_a.set_status('Starting conversion\u2026')

        def _log(l):
            parent.after(0, lambda x=str(l): app._log('[PFS] ' + x + '\n'))

        def worker():
            stats = {}
            def _log_parse(l):
                _log(l)
                _parse_summary(l, stats)

            try:
                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    parent.after(0, lambda: (
                        prog_a.set_status('mkpfs not available.', error=True),
                        messagebox.showerror('mkpfs not available',
                            'mkpfs is not bundled in this build.')))
                    return

                argv = ['pack', 'file']
                if compress_a_var.get():
                    argv.append('--compress')
                if version_ps5_a_var.get():
                    argv += ['--version', 'PS5']
                if getattr(sys, 'frozen', False):
                    argv += ['--cpu-count', '1']
                argv += [src, out_path]

                _log('mkpfs ' + ' '.join(argv))

                def _on_progress(phase, pct, total, detail):
                    parent.after(0, lambda ph=phase, p=pct, d=detail:
                        prog_a.update(ph, p, d))

                rc = run_mkpfs(argv, log_cb=_log_parse, progress_cb=_on_progress)
                # mkpfs may adjust extension (.ffpfs vs .ffpfsc)
                actual_a = out_path
                for _ext in ('.ffpfs', '.ffpfsc'):
                    _cand = os.path.splitext(out_path)[0] + _ext
                    if os.path.exists(_cand):
                        actual_a = _cand
                        break
                file_ok = os.path.exists(actual_a)

                if rc == 0 or (rc == 1 and file_ok):
                    sz = os.path.getsize(actual_a) / 1024**3
                    parent.after(0, lambda s=dict(stats): (
                        prog_a.done(),
                        btn_a.config(state='normal'),
                        prog_a.set_status('Done \u2713  %.2f GB' % sz),
                        _show_summary_panel(panel_a, s, actual_a),
                        messagebox.showinfo('Done', 'PFS image written:\n' + actual_a)
                        if rc == 0 else
                        messagebox.showwarning('Done (verify warnings)',
                            'PFS image written:\n' + actual_a +
                            '\n\nVerify check reported differences \u2014 see OUTPUT LOG.')))
                else:
                    parent.after(0, lambda: (
                        prog_a.fail(),
                        btn_a.config(state='normal'),
                        prog_a.set_status('Failed (rc=%d) \u2014 see OUTPUT LOG' % rc, error=True),
                        messagebox.showerror('Failed',
                            'mkpfs exited with code %d.\n\nCheck OUTPUT LOG.' % rc)))
                    try:
                        if os.path.exists(out_path):
                            os.remove(out_path)
                    except Exception:
                        pass
            except Exception as e:
                _log('Pipeline error A: ' + str(e))
                parent.after(0, lambda e=e: (
                    prog_a.fail(),
                    btn_a.config(state='normal'),
                    prog_a.set_status('Error: ' + str(e)[:60], error=True),
                    messagebox.showerror('Error', str(e))))
            finally:
                state_a['busy'] = False

        threading.Thread(target=worker, daemon=True).start()

    # ════════════════════════════════════════════════════════════════
    # PANEL B — Dump folder → exFAT → .ffpfs (automatic pipeline)
    # ════════════════════════════════════════════════════════════════
    src_b_var       = tk.StringVar()
    outdir_b_var    = tk.StringVar()
    name_b_var      = tk.StringVar()
    inter_name_b_var = tk.StringVar()  # intermediate exfat name
    size_b_var      = tk.StringVar(value='')
    step1_b_var     = tk.StringVar(value='exfat')  # 'exfat' or 'ffpkg'
    compress_b_var    = tk.BooleanVar(value=True)
    version_ps5_b_var = tk.BooleanVar(value=True)

    card_b = tk.Frame(panel_b, bg=COLORS['bg_2'],
                      highlightthickness=1,
                      highlightbackground=COLORS['border_2'])
    card_b.pack(fill='x', padx=24, pady=(0, 14))

    _card_head(card_b, '\U0001f5c2', 'Dump folder  \u2192  exFAT  \u2192  .ffpfs',
               'Builds an intermediate exFAT image then converts it to PFS automatically')

    body_b = tk.Frame(card_b, bg=COLORS['bg_2'])
    body_b.pack(fill='x', padx=24, pady=(4, 18))

    def _browse_src_b():
        p = filedialog.askdirectory(title='Select game dump folder')
        if p:
            src_b_var.set(p)

    def _browse_out_b():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            outdir_b_var.set(p)

    field_block(body_b, 'Game dump folder',
                var=src_b_var, on_browse=_browse_src_b,
                hint='folder containing eboot.bin / sce_sys')
    field_block(body_b, 'Output folder',
                var=outdir_b_var, on_browse=_browse_out_b,
                hint='both intermediate exFAT and final .ffpfs go here')
    field_block(body_b, 'Output name (.ffpfs)',
                var=name_b_var,
                hint='auto-filled from folder name if blank')

    # Intermediate step selector
    step_row = tk.Frame(body_b, bg=COLORS['bg_2'])
    step_row.pack(fill='x', pady=(8, 0))
    tk.Label(step_row, text='Intermediate format:',
             font=FONTS['label'], bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')
    for text, val in (('exFAT', 'exfat'), ('ffpkg', 'ffpkg')):
        rb = tk.Radiobutton(step_row, text=text, variable=step1_b_var, value=val,
                             font=FONTS['mono_sm'],
                             bg=COLORS['bg_2'], fg=COLORS['teal'],
                             selectcolor=COLORS['bg_3'],
                             activebackground=COLORS['bg_2'],
                             activeforeground=COLORS['teal'],
                             bd=0, padx=10, cursor='hand2',
                             indicatoron=True)
        rb.pack(side='left', padx=(10, 0))

    # Size estimate — calculated in background on folder select
    size_b_lbl = tk.Label(body_b, textvariable=size_b_var,
                           font=FONTS['mono_sm'],
                           bg=COLORS['bg_2'], fg=COLORS['teal'],
                           anchor='w', justify='left')
    size_b_lbl.pack(fill='x', pady=(10, 4))

    def _on_src_b(*_):
        p = src_b_var.get().strip()
        if not p or not os.path.isdir(p):
            return
        if not outdir_b_var.get():
            outdir_b_var.set(p)
        base = os.path.basename(p.rstrip('/\\'))
        if not name_b_var.get():
            name_b_var.set(base + '.ffpfs')
        if not inter_name_b_var.get():
            inter_name_b_var.set(base + '.exfat')
        size_b_var.set('Calculating folder size\u2026')

        def _calc():
            sz  = _get_folder_size(p)
            cnt = _count_files(p)
            # exFAT image is ~5% larger than raw data (alignment overhead)
            exfat_est = sz * 1.05
            # PFS compressed is typically 5-15% smaller than exFAT
            pfs_est_lo = exfat_est * 0.85
            pfs_est_hi = exfat_est * 0.95
            msg = (
                'Dump: %s  \u00b7  %d files\n'
                'exFAT intermediate: ~%s\n'
                'Final .ffpfs estimate: ~%s \u2013 %s  (after compression)'
            ) % (_fmt_size(sz), cnt,
                 _fmt_size(exfat_est),
                 _fmt_size(pfs_est_lo), _fmt_size(pfs_est_hi))
            parent.after(0, lambda: size_b_var.set(msg))

        threading.Thread(target=_calc, daemon=True).start()

    src_b_var.trace_add('write', _on_src_b)

    # Options
    opts_b = tk.Frame(body_b, bg=COLORS['bg_2'])
    opts_b.pack(fill='x', pady=(8, 0))
    tk.Label(opts_b, text='PFS options:', font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')
    for text, var in (('--compress', compress_b_var), ('--version PS5', version_ps5_b_var)):
        cb = _make_cb(opts_b, text, var)
        cb.pack(side='left', padx=(10 if text == '--compress' else 0, 0))

    # ── Progress block for panel B (4 stages) ────────────────────────
    prog_b = _ProgressBlock(body_b, _STAGES_FULL)

    btn_b = make_themed_button(body_b, text='Build exFAT then convert to .ffpfs',
                                command=lambda: _run_full_b(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=18, pady=9)
    btn_b.pack(anchor='w', pady=(14, 0))

    state_b = {'busy': False}

    def _run_full_b():
        if state_b['busy']:
            return
        src    = src_b_var.get().strip()
        outdir = outdir_b_var.get().strip()
        name   = name_b_var.get().strip()

        if not src or not os.path.isdir(src):
            messagebox.showerror('Source missing', 'Pick a valid game dump folder.')
            return
        if not os.path.isfile(os.path.join(src, 'eboot.bin')):
            if not messagebox.askyesno('eboot.bin not found',
                    'eboot.bin not found in the selected folder.\n\n'
                    'This may not be a valid PS5 game dump.\n\nContinue anyway?'):
                return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Output folder missing', 'Pick an output folder.')
            return
        if not name:
            messagebox.showerror('Output name missing', 'Set a .ffpfs output name.')
            return
        if not name.lower().endswith('.ffpfs'):
            name += '.ffpfs'

        base     = os.path.splitext(name)[0]
        step_fmt = step1_b_var.get()
        inter    = os.path.normpath(os.path.join(outdir, base + '.' + step_fmt))
        out_path = os.path.normpath(os.path.join(outdir, name))

        for p, lbl in ((inter, 'Intermediate'), (out_path, 'Output')):
            if os.path.exists(p):
                if not messagebox.askyesno('Overwrite',
                        lbl + ' file exists:\n' + p + '\n\nOverwrite?'):
                    return
                try:
                    os.remove(p)
                except Exception as e:
                    messagebox.showerror('Error', 'Cannot remove: ' + str(e))
                    return

        state_b['busy'] = True
        btn_b.config(state='disabled')
        prog_b.reset()
        prog_b.set_status('Starting\u2026')

        def _log(l):
            parent.after(0, lambda x=str(l): app._log('[PFS] ' + x + '\n'))

        def worker():
            stats = {}
            def _log_parse(l):
                _log(l)
                _parse_summary(l, stats)

            try:
                # ── Step 1: scan (size + file count display) ──────────
                parent.after(0, lambda: (
                    prog_b.update('scan', 0, 'Counting files\u2026'),))
                sz  = _get_folder_size(src)
                cnt = _count_files(src)
                parent.after(0, lambda: (
                    prog_b.update('scan', 50,
                        '%d files  \u00b7  %s' % (cnt, _fmt_size(sz))),))
                parent.after(0, lambda: (
                    prog_b.update('scan', 100, 'Scan complete'),))

                # ── Step 2: build exFAT (or ffpkg) ────────────────────
                parent.after(0, lambda f=step_fmt: (
                    prog_b.update('exfat', 0,
                        'Building %s image\u2026' % f.upper()),))

                # Check required tools
                osfmount = None
                try:
                    osfmount = getattr(app, '_find_osfmount',
                        lambda: app._settings.get('osfmount_path', ''))()
                except Exception:
                    pass
                if not osfmount or not os.path.isfile(osfmount):
                    parent.after(0, lambda: (
                        prog_b.fail(),
                        btn_b.config(state='normal'),
                        prog_b.set_status('OSFMount not found \u2014 required for exFAT build', error=True),
                        messagebox.showerror('OSFMount missing',
                            'OSFMount is required to build the exFAT image.\n\n'
                            'Set the path in Settings \u2192 OSFMount.')))
                    return

                bat_path = getattr(app, '_bat_path', None)
                if not bat_path or not os.path.isfile(bat_path):
                    parent.after(0, lambda: (
                        prog_b.fail(),
                        btn_b.config(state='normal'),
                        prog_b.set_status('Build scripts not found', error=True),
                        messagebox.showerror('Build scripts missing',
                            'The exFAT build scripts are missing.\n'
                            'Try restarting the app.')))
                    return

                # Run the exFAT build script
                CREATE_NO_WINDOW = 0x08000000
                kwargs = {'creationflags': CREATE_NO_WINDOW} if os.name == 'nt' else {}
                osf_path     = osfmount
                cluster_arg  = getattr(app, '_get_cluster_size_arg',  lambda: '65536')()
                sector_arg   = getattr(app, '_get_sector_size_arg',   lambda: '512')()
                threads_arg  = getattr(app, '_get_threads_arg',       lambda: '8')()
                retries_arg  = '3'
                rwait_arg    = '3'
                excl_hidden  = '0'
                img_override = ''

                cmd = ('"%s" "%s" "%s" "%s" "%s" "%s" "%s" "%s" "%s" "%s" "%s"' % (
                    bat_path, inter, src, osf_path, cluster_arg, sector_arg,
                    threads_arg, retries_arg, rwait_arg, excl_hidden, img_override))
                _log('Building exFAT: ' + cmd)

                # Robocopy step weights within the exfat stage (0-100)
                # [1/4] create+mount=2%, [2/4] format=5%, [3/4] copy=95%, [4/4] unmount=100%
                _ROBO_STEPS = {
                    '1/4': (0,   2,  'Creating image'),
                    '2/4': (2,   5,  'Formatting exFAT'),
                    '3/4': (5,  97,  'Copying files'),
                    '4/4': (97, 100, 'Unmounting'),
                }
                _last_step = [None]

                def _handle_robo_line(raw):
                    line = raw.replace('\r', '').strip()
                    if not line:
                        return
                    # Skip bare percentage lines (per-file noise before transfer starts)
                    if re.match(r'^\d{1,3}%$', line):
                        return
                    _log(raw)
                    # Step marker [N/4]
                    sm = re.search(r'\[(\d)/4\]', line)
                    if sm:
                        key = sm.group(1) + '/4'
                        if key in _ROBO_STEPS:
                            _last_step[0] = key
                            lo, _, label = _ROBO_STEPS[key]
                            parent.after(0, lambda p=lo, l=label:
                                prog_b.update('exfat', p, l))
                        return
                    # Robocopy copy progress: "  45%  ETA 0:02:15"
                    pm = re.search(r'(\d{1,3})%', line)
                    if pm and _last_step[0] == '3/4':
                        robo_pct = int(pm.group(1))
                        lo, hi, _ = _ROBO_STEPS['3/4']
                        mapped = int(lo + (hi - lo) * robo_pct / 100.0)
                        eta_m = re.search(r'(\d+):(\d{2}):(\d{2})', line)
                        if eta_m:
                            h, mn, sc = int(eta_m.group(1)), int(eta_m.group(2)), int(eta_m.group(3))
                            total = h * 3600 + mn * 60 + sc
                            detail = ('Almost done' if total < 5 else
                                      'ETA %dh %02dm %02ds' % (h, mn, sc) if h else
                                      'ETA %dm %02ds' % (mn, sc))
                        else:
                            detail = 'Copying files  %d%%' % robo_pct
                        parent.after(0, lambda p=mapped, d=detail:
                            prog_b.update('exfat', p, d))

                try:
                    proc = subprocess.Popen(
                        cmd, shell=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, errors='replace', **kwargs)

                    for line in proc.stdout:
                        _handle_robo_line(line)
                    proc.wait()
                    rc_exfat = proc.returncode
                except Exception as e:
                    _log('exFAT build error: ' + str(e))
                    rc_exfat = -1

                if rc_exfat != 0 or not os.path.isfile(inter):
                    parent.after(0, lambda: (
                        prog_b.fail(),
                        btn_b.config(state='normal'),
                        prog_b.set_status('exFAT build failed (rc=%d)' % rc_exfat, error=True),
                        messagebox.showerror('exFAT build failed',
                            'The exFAT build script returned code %d.\n\n'
                            'Check OUTPUT LOG for details.' % rc_exfat)))
                    return

                inter_sz = os.path.getsize(inter)
                parent.after(0, lambda: (
                    prog_b.update('exfat', 100,
                        'exFAT ready  \u00b7  %s' % _fmt_size(inter_sz)),))

                # ── Step 3+4: convert exFAT → .ffpfs ──────────────────
                parent.after(0, lambda: (
                    prog_b.update('compress', 0, 'Starting PFS compression\u2026'),))

                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    parent.after(0, lambda: (
                        prog_b.fail(),
                        btn_b.config(state='normal'),
                        prog_b.set_status('mkpfs not available', error=True)))
                    return

                argv = ['pack', 'file']
                if compress_b_var.get():
                    argv.append('--compress')
                if version_ps5_b_var.get():
                    argv += ['--version', 'PS5']
                if getattr(sys, 'frozen', False):
                    argv += ['--cpu-count', '1']
                argv += [inter, out_path]
                _log('mkpfs ' + ' '.join(argv))

                def _on_progress(phase, pct, total, detail):
                    parent.after(0, lambda ph=phase, p=pct, d=detail:
                        prog_b.update(ph, p, d))

                rc_pfs = run_mkpfs(argv, log_cb=_log_parse, progress_cb=_on_progress)

                # mkpfs auto-adjusts extension (.ffpfs for game folders,
                # .ffpfsc for single-file container). Find actual output.
                actual_b = out_path
                for _ext in ('.ffpfs', '.ffpfsc'):
                    _cand = os.path.normpath(
                        os.path.splitext(out_path)[0] + _ext)
                    if os.path.exists(_cand):
                        actual_b = _cand
                        break
                file_ok = os.path.exists(actual_b)

                # Clean up intermediate
                try:
                    if os.path.isfile(inter):
                        os.remove(inter)
                        _log('Removed intermediate: ' + inter)
                except Exception:
                    pass

                if rc_pfs == 0 or (rc_pfs == 1 and file_ok):
                    sz_gb = os.path.getsize(actual_b) / 1024**3
                    parent.after(0, lambda s=dict(stats): (
                        prog_b.done(),
                        btn_b.config(state='normal'),
                        prog_b.set_status('Done \u2713  %.2f GB' % sz_gb),
                        _show_summary_panel(panel_b, s, actual_b),
                        messagebox.showinfo('Done', 'PFS image written:\n' + actual_b)
                        if rc_pfs == 0 else
                        messagebox.showwarning('Done (verify warnings)',
                            'PFS image written:\n' + actual_b +
                            '\n\nVerify check reported differences \u2014 see OUTPUT LOG.')))
                else:
                    parent.after(0, lambda: (
                        prog_b.fail(),
                        btn_b.config(state='normal'),
                        prog_b.set_status('PFS conversion failed (rc=%d)' % rc_pfs, error=True),
                        messagebox.showerror('PFS failed',
                            'mkpfs exited with code %d.\n\nCheck OUTPUT LOG.' % rc_pfs)))
                    try:
                        if os.path.exists(out_path):
                            os.remove(out_path)
                    except Exception:
                        pass

            except Exception as e:
                _log('Pipeline error: ' + str(e))
                parent.after(0, lambda e=e: (
                    prog_b.fail(),
                    btn_b.config(state='normal'),
                    prog_b.set_status('Error: ' + str(e)[:60], error=True),
                    messagebox.showerror('Pipeline error', str(e))))
            finally:
                state_b['busy'] = False

        threading.Thread(target=worker, daemon=True).start()

    # ════════════════════════════════════════════════════════════════
    # PANEL C — Extract .ffpfs / .ffpfsc → folder
    # ════════════════════════════════════════════════════════════════
    src_c_var    = tk.StringVar()
    outdir_c_var = tk.StringVar()
    info_c_var   = tk.StringVar(value='')

    card_c = tk.Frame(panel_c, bg=COLORS['bg_2'],
                      highlightthickness=1,
                      highlightbackground=COLORS['border_2'])
    card_c.pack(fill='x', padx=24, pady=(0, 14))

    _card_head(card_c, '\U0001f4e4', 'Extract .ffpfs  \u2192  folder',
               'Unpack a PFS image so you can edit files, then rebuild via Convert or Build')

    body_c = tk.Frame(card_c, bg=COLORS['bg_2'])
    body_c.pack(fill='x', padx=24, pady=(4, 18))

    # Read-only notice
    note_c = tk.Frame(body_c, bg=COLORS['bg_3'],
                      highlightthickness=1, highlightbackground=COLORS['border_3'])
    note_c.pack(fill='x', pady=(0, 12))
    tk.Label(note_c,
             text=('\u2139  PFS images are read-only \u2014 files cannot be changed in place.\n'
                   'To edit: extract here \u2192 modify the folder \u2192 rebuild from the Build tab.'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_3'],
             anchor='w', justify='left').pack(fill='x', padx=12, pady=8)

    def _browse_src_c():
        p = filedialog.askopenfilename(
            title='Select PFS image',
            filetypes=[('PFS images', '*.ffpfs *.ffpfsc'),
                       ('All files', '*.*')])
        if p:
            src_c_var.set(p)

    def _browse_out_c():
        p = filedialog.askdirectory(title='Select destination folder')
        if p:
            outdir_c_var.set(p)

    field_block(body_c, 'PFS image (.ffpfs / .ffpfsc)',
                var=src_c_var, on_browse=_browse_src_c,
                hint='the image to extract')
    field_block(body_c, 'Extract to folder',
                var=outdir_c_var, on_browse=_browse_out_c,
                hint='a subfolder is created from the image name')

    def _on_src_c(*_):
        p = src_c_var.get().strip()
        if not p or not os.path.isfile(p):
            return
        if not outdir_c_var.get():
            outdir_c_var.set(os.path.dirname(p))
        try:
            sz = os.path.getsize(p)
            info_c_var.set('Image: %s  \u2014  files will be uncompressed on extract'
                           % _fmt_size(sz))
        except Exception:
            info_c_var.set('')
    src_c_var.trace_add('write', _on_src_c)

    tk.Label(body_c, textvariable=info_c_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['teal'],
             anchor='w').pack(fill='x', pady=(4, 8))

    prog_c = _ProgressBlock(body_c, [('extract', 'Extract', 0, 100)])

    btn_c = make_themed_button(body_c, text='Extract to folder',
                                command=lambda: _run_extract_c(),
                                kind='success', icon='\u25b6',
                                font_size=10, padx=18, pady=9)
    btn_c.pack(anchor='w', pady=(14, 0))

    state_c = {'busy': False}

    def _run_extract_c():
        if state_c['busy']:
            return
        src    = src_c_var.get().strip()
        outdir = outdir_c_var.get().strip()
        if not src or not os.path.isfile(src):
            messagebox.showerror('Source missing', 'Pick a valid .ffpfs or .ffpfsc image.')
            return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Folder missing', 'Pick a destination folder.')
            return

        # Create a subfolder named after the image stem
        stem = os.path.splitext(os.path.basename(src))[0]
        dest = os.path.normpath(os.path.join(outdir, stem))

        overwrite = False
        if os.path.exists(dest):
            if not messagebox.askyesno('Folder exists',
                    dest + '\n\nalready exists. Overwrite its contents?'):
                return
            overwrite = True

        state_c['busy'] = True
        btn_c.config(state='disabled')
        prog_c.reset()
        prog_c.update('extract', 0, 'Starting extraction\u2026')

        def _log(l):
            parent.after(0, lambda x=str(l): app._log('[PFS] ' + x + '\n'))

        def worker():
            try:
                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    parent.after(0, lambda: (
                        prog_c.fail(),
                        btn_c.config(state='normal'),
                        prog_c.set_status('mkpfs not available', error=True),
                        messagebox.showerror('mkpfs not available',
                            'mkpfs is not bundled in this build.')))
                    return

                argv = ['unpack', src, dest]
                if overwrite:
                    argv.append('--overwrite')
                _log('mkpfs ' + ' '.join(argv))

                # extract emits progress lines similarly to pack
                def _on_progress(phase, pct, total, detail):
                    parent.after(0, lambda p=pct, d=detail:
                        prog_c.update('extract', p, d))

                rc = run_mkpfs(argv, log_cb=_log, progress_cb=_on_progress)

                if rc == 0 and os.path.isdir(dest):
                    # Count what we got
                    n = _count_files(dest)
                    sz = _get_folder_size(dest)
                    parent.after(0, lambda: (
                        prog_c.done(),
                        btn_c.config(state='normal'),
                        prog_c.set_status('Extracted %d files  \u00b7  %s' % (n, _fmt_size(sz))),
                        messagebox.showinfo('Extraction complete',
                            'Extracted to:\n' + dest +
                            '\n\n%d files  \u00b7  %s\n\n'
                            'Edit the files, then rebuild from the Build tab.'
                            % (n, _fmt_size(sz)))))
                else:
                    parent.after(0, lambda: (
                        prog_c.fail(),
                        btn_c.config(state='normal'),
                        prog_c.set_status('Extraction failed (rc=%d)' % rc, error=True),
                        messagebox.showerror('Extraction failed',
                            'mkpfs exited with code %d.\n\nCheck OUTPUT LOG.' % rc)))
            except Exception as e:
                _log('Extract error: ' + str(e))
                parent.after(0, lambda e=e: (
                    prog_c.fail(),
                    btn_c.config(state='normal'),
                    prog_c.set_status('Error: ' + str(e)[:60], error=True),
                    messagebox.showerror('Extract error', str(e))))
            finally:
                state_c['busy'] = False

        threading.Thread(target=worker, daemon=True).start()

    # ── Initial panel display ─────────────────────────────────────────
    _refresh_panels()


# ════════════════════════════════════════════════════════════════════
# Shared helpers
# ════════════════════════════════════════════════════════════════════

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
