"""ui/tab_convert.py — Convert tab (v2: spacious Build-tab style).

Layout:

    ┌─ Page head (badge + title + subtitle) ────────────────────────────┐
    │ [💿]  Convert images                                              │
    │       Convert between .exfat and .ffpkg                           │
    ├───────────────────────────────────────────────────────────────────┤
    │ [i] Conversion mounts the source, runs UFS2Tool newfs against...  │
    ├───────────────────────────────────────────────────────────────────┤
    │ ┌─ Card: exFAT → ffpkg ─────────────────────────────────────────┐ │
    │ │ Source .exfat                                                 │ │
    │ │ [_______________________________________________]   [Browse]  │ │
    │ │ Output folder / Output name / [▶ Convert to ffpkg]            │ │
    │ └───────────────────────────────────────────────────────────────┘ │
    │                                                                   │
    │ ┌─ Card: ffpkg → exFAT ─────────────────────────────────────────┐ │
    │ │ Source .ffpkg                                                 │ │
    │ │ [_______________________________________________]   [Browse]  │ │
    │ │ Output folder / Output name / [▶ Convert to exFAT]            │ │
    │ └───────────────────────────────────────────────────────────────┘ │
    └───────────────────────────────────────────────────────────────────┘

Output goes to the global OUTPUT LOG at the bottom (no embedded log
duplicating it).

The ffpkg → exFAT direction extracts the source via UFS2Tool's
recursive extract (which preserves empty directories — see the Apply
backport flow's v2.0.6f notes), creates a blank fixed-size .exfat
file sized at ~110% of the extracted contents, mounts it via
OSFMount, formats it with Windows' `format /FS:exFAT /Q`, robocopies
the dump in, then dismounts.
"""

import os
import re
import subprocess
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

from tkinter_theme import COLORS, FONTS
from ui.shared.page_head import (
    make_themed_button, info_banner, page_head, field_block)
from ui.shared.hero import GameHero


def _flow_chips(parent, src_ext, dst_ext):
    """Compact Source → Destination flow chips, packed right in a
    card head (v3.6.0 pass)."""
    wrap = tk.Frame(parent, bg=COLORS['bg_2'])
    wrap.pack(side='right', padx=(8, 0))
    tk.Label(wrap, text=' ' + src_ext + ' ',
             font=(FONTS['mono_sm'][0], 9, 'bold'),
             bg=COLORS['accent_08'], fg=COLORS['accent_hi'],
             padx=6, pady=2,
             highlightbackground=COLORS['accent_lo'],
             highlightthickness=1).pack(side='left')
    tk.Label(wrap, text='\u2192',
             font=(FONTS['body'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_4']
             ).pack(side='left', padx=6)
    tk.Label(wrap, text=' ' + dst_ext + ' ',
             font=(FONTS['mono_sm'][0], 9, 'bold'),
             bg=COLORS['teal_bg'], fg=COLORS['teal_hi'],
             padx=6, pady=2,
             highlightbackground=COLORS['border_3'],
             highlightthickness=1).pack(side='left')


def build_convert_tab(parent, app):
    """Build the redesigned Convert tab. `app` is the App instance."""
    parent.configure(bg=COLORS['bg_1'])

    # ── State ──
    e2f_src    = tk.StringVar()
    e2f_outdir = tk.StringVar()
    e2f_name   = tk.StringVar()
    e2f_status_var = tk.StringVar(value='Idle.')
    f2e_src    = tk.StringVar()
    f2e_outdir = tk.StringVar()
    f2e_name   = tk.StringVar()
    f2e_status_var = tk.StringVar(value='Idle.')
    # Shared: only one conversion runs at a time.
    state = {'busy': False}
    # Back-compat alias for old code below — points at whichever card
    # is currently active.
    status_var = e2f_status_var

    # ── Scrollable wrap ──
    canvas = tk.Canvas(parent, bg=COLORS['bg_1'], bd=0,
                       highlightthickness=0)
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

    # ── Page head with badge ──
    head = page_head(inner, '\U0001f4bf',
                     'Convert images',
                     'Convert between .exfat and .ffpkg.')
    head.pack(fill='x', padx=24, pady=(14, 12))

    # Right-aligned Force Dismount button on the page head row
    def _force_dismount():
        try:
            app._force_dismount_all()
        except Exception as e:
            messagebox.showerror('Force Dismount failed', str(e))

    fd_btn = make_themed_button(head,
                                  '\u26a0  Force Dismount',
                                  command=_force_dismount,
                                  kind='ghost')
    fd_btn.pack(side='right', padx=(8, 0))

    # ── Info banner ──
    banner = info_banner(inner,
        'Conversion mounts the source via OSFMount, runs UFS2Tool '
        'newfs against the mount point, then unmounts. Output goes '
        'to the global OUTPUT LOG (click at the bottom to expand).')
    banner.pack(fill='x', padx=24, pady=(0, 14))

    # ── Selected-image hero (v3.6.0 pass) ──
    # Hidden until a source file is picked in either card; then shows
    # the game parsed from the image's filename, the source/output
    # formats, the file size, and a READY TO CONVERT badge.
    # Presentation only — built entirely from the path string and
    # os.path.getsize; nothing is mounted or opened.
    hero = GameHero(inner,
                    stats=[('Source Format', 'src'),
                           ('Output Format', 'dst'),
                           ('Size', 'size'),
                           ('Status', 'status')],
                    cover_glyph='\U0001f4bf', cover_size=120)
    hero_packed = {'on': False}

    def _humansize(n):
        try:
            if n >= 1024**3:
                return '%.2f GB' % (n / 1024**3)
            return '%d MB' % (n // 1024**2)
        except Exception:
            return '\u2014'

    def _update_hero(path, src_fmt, dst_fmt):
        try:
            if not path or not os.path.isfile(path):
                if hero_packed['on']:
                    hero.pack_forget()
                    hero_packed['on'] = False
                return
            if not hero_packed['on']:
                hero.pack(fill='x', padx=24, pady=(0, 14), after=banner)
                hero_packed['on'] = True
            from ui.tab_ps5_mgr import parse_meta_from_filename
            gid, ver, disp = parse_meta_from_filename(
                os.path.basename(path))
            title = disp or os.path.splitext(os.path.basename(path))[0]
            hero.set_title(title, (gid or '') +
                           ((' \u00b7 v' + ver) if ver else ''))
            hero.set_path(path)
            hero.set_stat('src', src_fmt)
            hero.set_stat('dst', dst_fmt)
            try:
                hero.set_stat('size', _humansize(os.path.getsize(path)))
            except Exception:
                hero.set_stat('size', '\u2014')
            hero.set_stat('status', 'Ready')
            hero.set_badge('READY TO CONVERT', 'ready')
            hero.reset_cover()
        except Exception:
            pass
    app._conv_update_hero = _update_hero

    # ── Conversion card ──
    card_outer = tk.Frame(inner, bg=COLORS['bg_2'],
                           highlightbackground=COLORS['border_2'],
                           highlightthickness=1)
    card_outer.pack(fill='x', padx=24, pady=(0, 14))

    # Card head
    chead = tk.Frame(card_outer, bg=COLORS['bg_2'])
    chead.pack(fill='x', padx=24, pady=(18, 14))

    # Icon tile
    ico = tk.Label(chead, text='\u2192',
                   font=(FONTS['h2'][0], 13),
                   bg=COLORS['accent_08'], fg=COLORS['accent'],
                   width=2, padx=4, pady=2)
    ico.pack(side='left', padx=(0, 12))

    _flow_chips(chead, '.exfat', '.ffpkg')

    title_col = tk.Frame(chead, bg=COLORS['bg_2'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text='exFAT \u2192 ffpkg',
             font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(fill='x')
    tk.Label(title_col,
             text='Pick an existing .exfat image and a destination.',
             font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w'
             ).pack(fill='x', pady=(2, 0))

    # Hairline under head
    tk.Frame(card_outer, bg=COLORS['border_2'], height=1
             ).pack(fill='x')

    # Card body
    body = tk.Frame(card_outer, bg=COLORS['bg_2'])
    body.pack(fill='x', padx=24, pady=(4, 18))

    # ── Form fields ──
    def _browse_src():
        p = filedialog.askopenfilename(
            title='Select .exfat image',
            filetypes=[('exFAT images', '*.exfat'),
                       ('All files', '*.*')])
        if p:
            e2f_src.set(p)

    def _browse_outdir():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            e2f_outdir.set(p)

    field_block(body, 'Source .exfat',
                 var=e2f_src, on_browse=_browse_src,
                 hint='the image to convert')
    field_block(body, 'Output folder',
                 var=e2f_outdir, on_browse=_browse_outdir,
                 hint='where the .ffpkg will be written')
    field_block(body, 'Output name',
                 var=e2f_name,
                 hint='auto-filled from source if blank')

    # Auto-fill name & outdir when source is picked
    def _on_e2f_src(*_a):
        if e2f_src.get() and not e2f_name.get():
            base = os.path.splitext(os.path.basename(e2f_src.get()))[0]
            e2f_name.set(base + '.ffpkg')
        if e2f_src.get() and not e2f_outdir.get():
            e2f_outdir.set(os.path.dirname(e2f_src.get()))
        _update_hero(e2f_src.get().strip(), 'exFAT', 'ffpkg')
    e2f_src.trace_add('write', _on_e2f_src)

    # ── Action row: Convert button + status + progress bar ──
    action_row = tk.Frame(body, bg=COLORS['bg_2'])
    action_row.pack(fill='x', pady=(18, 0))

    convert_btn = make_themed_button(
        action_row,
        text='Convert to ffpkg',
        command=lambda: _do_exfat_to_ffpkg(),
        kind='success',
        icon='\u25b6',
        font_size=10, padx=18, pady=9)
    convert_btn.pack(side='left')

    status_lbl = tk.Label(action_row, textvariable=e2f_status_var,
                          font=FONTS['mono_sm'],
                          bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                          anchor='w')
    status_lbl.pack(side='left', padx=(16, 0))

    # Slim progress bar on the right of the action row
    pbar_wrap = tk.Frame(action_row, bg=COLORS['bg_2'])
    pbar_wrap.pack(side='right', fill='x', expand=True, padx=(16, 0))
    pbar = ttk.Progressbar(pbar_wrap, mode='indeterminate', length=200)
    pbar.pack(fill='x')

    # ── Helpers ──
    def _log(line):
        """Funnel everything to the global OUTPUT LOG drawer."""
        parent.after(0, lambda l=str(line): app._log('[CONVERT] ' + l.rstrip() + '\n'))

    def _set_busy_e2f(b, label=''):
        state['busy'] = b
        try:
            if b:
                pbar.start(10)
                e2f_status_var.set(label or 'Working...')
                convert_btn.config(state='disabled', cursor='watch')
                # Also lock the other card's button so the user can't
                # try to launch a concurrent run.
                if 'f2e_btn' in state and state['f2e_btn']:
                    state['f2e_btn'].config(state='disabled')
            else:
                pbar.stop()
                e2f_status_var.set(label or 'Idle.')
                convert_btn.config(state='normal', cursor='hand2')
                if 'f2e_btn' in state and state['f2e_btn']:
                    state['f2e_btn'].config(state='normal')
        except Exception:
            pass

    # Back-compat alias for the existing code path below.
    _set_busy = _set_busy_e2f

    def _get_ufs2tool_exe():
        try:
            from exfat_builder import extract_ufs2tool, _UFS2TOOL_DIR
            if _UFS2TOOL_DIR and os.path.isdir(_UFS2TOOL_DIR):
                exe = os.path.join(_UFS2TOOL_DIR, 'UFS2Tool.exe')
                if os.path.isfile(exe):
                    return exe
            return extract_ufs2tool(
                getattr(app, '_settings', {}).get('temp_dir') or None)
        except Exception as e:
            _log('Failed to extract UFS2Tool: %s' % e)
            return None

    def _run(cmd, label=None, progress_cb=None):
        """Run a subprocess, stream output to the global log, return rc.

        If `progress_cb` is provided, it's invoked for every output
        line so the caller can parse progress (UFS2Tool's
        `Adding files... NN%`, robocopy's per-file output, etc.).
        Exceptions inside the callback are swallowed so progress
        parsing bugs can't break the actual conversion."""
        if label:
            _log(label)
        CREATE_NO_WINDOW = 0x08000000
        try:
            kwargs = ({'creationflags': CREATE_NO_WINDOW}
                      if os.name == 'nt' else {})
            p = subprocess.Popen(cmd,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT,
                                  text=True, errors='replace',
                                  **kwargs)
            for ln in p.stdout:
                _log(ln.rstrip())
                if progress_cb is not None:
                    try:
                        progress_cb(ln.rstrip())
                    except Exception:
                        pass
            p.wait()
            return p.returncode
        except FileNotFoundError:
            _log('Command not found: ' + cmd[0])
            return -1
        except Exception as e:
            _log('Run error: ' + str(e))
            return -1

    # ── Main conversion ──
    def _do_exfat_to_ffpkg():
        if state['busy']:
            return
        src = e2f_src.get().strip()
        outdir = e2f_outdir.get().strip()
        name = e2f_name.get().strip()
        if not src or not os.path.isfile(src):
            messagebox.showerror('Source missing',
                'Pick a valid .exfat source image.')
            return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Output folder missing',
                'Pick an output folder.')
            return
        if not name:
            messagebox.showerror('Output name missing',
                'Set an output filename.')
            return
        if not name.lower().endswith('.ffpkg'):
            name = name + '.ffpkg'
        out_path = os.path.join(outdir, name)
        if os.path.exists(out_path):
            if not messagebox.askyesno('Overwrite',
                    out_path + '\n\nalready exists. Overwrite?'):
                return
            try:
                os.remove(out_path)
            except Exception as e:
                _log('Could not remove existing: ' + str(e))
                return

        # v3.0.0: the OUTPUT LOG is never auto-opened. The progress
        # dialog below carries the user-visible status; if the user
        # wants the full text log, they can click the OUTPUT LOG
        # toggle at the bottom of the window themselves.

        # v3.0.0: show the rich progress dialog instead of the
        # action-row indeterminate bar. Stage weights are calibrated
        # for the exFAT → ffpkg flow (newfs dominates the time).
        from ui.tab_ffpkg_edit import _RebuildProgress
        prog = _RebuildProgress(parent, 'Converting exFAT → ffpkg',
            weights={
                'mount':    (0,   5),
                'newfs':    (5,  97),
                'dismount': (97, 100),
            },
            initial_stage='mount')

        _set_busy_e2f(True, 'Locating tools...')

        def worker():
            ufs2 = _get_ufs2tool_exe()
            if not ufs2:
                parent.after(0, prog.close)
                parent.after(0, lambda: _set_busy_e2f(False, 'Failed.'))
                parent.after(0, lambda: messagebox.showerror(
                    'UFS2Tool not available',
                    'UFS2Tool could not be extracted. Conversion '
                    'aborted.'))
                return

            # Locate OSFMount via app helpers / settings
            osfmount = None
            try:
                osfmount = getattr(app, '_find_osfmount',
                                    lambda: None)()
            except Exception:
                osfmount = None
            if not osfmount or not os.path.isfile(osfmount):
                _log('OSFMount not found.')
                parent.after(0, prog.close)
                parent.after(0, lambda: _set_busy_e2f(False, 'Failed.'))
                parent.after(0, lambda: messagebox.showerror(
                    'OSFMount missing',
                    'OSFMount is required to convert exFAT to ffpkg.\n'
                    'Configure its path under Settings.'))
                return

            # Find a free drive letter
            import ctypes as _ct
            used_mask = _ct.windll.kernel32.GetLogicalDrives()
            mount_letter = None
            for code in range(ord('G'), ord('Z') + 1):
                if not (used_mask & (1 << (code - ord('A')))):
                    mount_letter = chr(code) + ':'
                    break
            if not mount_letter:
                _log('No free drive letter for mount.')
                parent.after(0, prog.close)
                parent.after(0, lambda: _set_busy_e2f(False, 'Failed.'))
                return

            _log('Mounting %s at %s ...' % (src, mount_letter))
            parent.after(0, prog.set_stage, 'mount',
                'Mounting source image...')

            mount_cmd = [osfmount, '-a', '-t', 'file', '-f', src,
                         '-m', mount_letter, '-o', 'rw']
            rc = _run(mount_cmd)
            if rc != 0:
                _log('Mount failed (rc=%d).' % rc)
                parent.after(0, prog.close)
                parent.after(0, lambda:
                    _set_busy_e2f(False, 'Mount failed.'))
                return
            parent.after(0, prog.set_stage_progress, 100.0)

            try:
                parent.after(0, prog.set_stage, 'newfs',
                    'Building .ffpkg with UFS2Tool newfs...')
                _log('UFS2Tool newfs against mount...')
                newfs_cmd = [ufs2, 'newfs',
                             '-O', '2',
                             '-b', '32768',
                             '-f', '4096',
                             '-S', '512',
                             '-D', mount_letter + '\\',
                             out_path]

                # Parse newfs output for progress. Two sub-phases:
                # "Writing cylinder groups... NN%" and then
                # "Adding files... NN% (x/y files, X GiB/Y GiB)".
                newfs_state = {'sub': 'init'}
                def _newfs_cb(line):
                    stripped = line.strip()
                    if 'Writing cylinder groups' in stripped:
                        newfs_state['sub'] = 'init'
                        parent.after(0, prog.set_detail,
                            'Writing filesystem structure...')
                    elif 'Adding files to image' in stripped:
                        newfs_state['sub'] = 'files'
                        parent.after(0, prog.set_detail,
                            'Copying files into image...')
                    elif ('Populated image with' in stripped
                          or 'Image created successfully' in stripped):
                        parent.after(0, prog.set_stage_progress, 100.0,
                            'Image created, finalising...')
                        return
                    m = re.search(r'(\d{1,3})\s*%', stripped)
                    if not m:
                        return
                    raw_pct = max(0, min(100, int(m.group(1))))
                    # File count + byte progress
                    mf = re.search(r'\((\d+)\s*/\s*(\d+)\s+files?',
                                    stripped)
                    files_done = files_total = 0
                    if mf:
                        files_done = int(mf.group(1))
                        files_total = int(mf.group(2))
                    mg = re.search(
                        r'([\d.]+)\s*GiB\s*/\s*([\d.]+)\s*GiB',
                        stripped)
                    written_gib = total_gib = 0.0
                    if mg:
                        written_gib = float(mg.group(1))
                        total_gib   = float(mg.group(2))
                    # Within the newfs stage, init goes 0–25%, files
                    # 25–100% — newfs spends most time on file copy.
                    if newfs_state['sub'] == 'init':
                        local_pct = raw_pct * 0.25
                        detail = ('Initialising filesystem... %d%%'
                                  % raw_pct)
                    else:
                        local_pct = 25 + raw_pct * 0.75
                        bits = []
                        if files_total:
                            bits.append('%d / %d files'
                                        % (files_done, files_total))
                        if total_gib:
                            bits.append('%.2f / %.2f GB'
                                        % (written_gib, total_gib))
                        detail = ('  •  '.join(bits) if bits
                                  else '%d%%' % raw_pct)
                    parent.after(0, prog.set_stage_progress,
                        local_pct, detail)

                rc = _run(newfs_cmd, progress_cb=_newfs_cb)
                if rc != 0:
                    _log('newfs failed (rc=%d).' % rc)
                    parent.after(0, prog.close)
                    parent.after(0, lambda:
                        _set_busy_e2f(False, 'newfs failed.'))
                    return
                _log('Built %s OK.' % out_path)
                parent.after(0, prog.set_stage_progress, 100.0,
                    'Image built.')
            finally:
                parent.after(0, prog.set_stage, 'dismount',
                    'Unmounting source...')
                _log('Unmounting %s ...' % mount_letter)
                # Use the robust dismount helper so we don't suffer
                # the same handle-busy issues that exFAT builds had
                # before the v2.5.7 dismount fix.
                try:
                    letter = mount_letter.rstrip(':\\')
                    if hasattr(app, '_dismount_drive_robust'):
                        app._dismount_drive_robust(letter,
                                                    max_wait_seconds=20)
                    else:
                        _run([osfmount, '-d', '-m', mount_letter])
                except Exception as e:
                    _log('Dismount error: ' + str(e))
                parent.after(0, prog.set_stage_progress, 100.0)

            parent.after(0, prog.close)
            parent.after(0, lambda: _set_busy_e2f(False, 'Done \u2713'))
            parent.after(0, lambda: messagebox.showinfo(
                'Convert complete',
                'Wrote:\n' + out_path))
            # v3.6.2: occasional support nudge (frequency-gated).
            def _nudge_cv():
                try:
                    from ui.release_notes import note_successful_operation
                    note_successful_operation(app, 'Convert')
                except Exception:
                    pass
            parent.after(700, _nudge_cv)
    f2e_card = tk.Frame(inner, bg=COLORS['bg_2'],
                         highlightbackground=COLORS['border_2'],
                         highlightthickness=1)
    f2e_card.pack(fill='x', padx=24, pady=(0, 14))

    f2e_chead = tk.Frame(f2e_card, bg=COLORS['bg_2'])
    f2e_chead.pack(fill='x', padx=24, pady=(18, 14))
    f2e_ico = tk.Label(f2e_chead, text='\u2192',
                       font=(FONTS['h2'][0], 13),
                       bg=COLORS['accent_08'], fg=COLORS['accent'],
                       width=2, padx=4, pady=2)
    f2e_ico.pack(side='left', padx=(0, 12))
    _flow_chips(f2e_chead, '.ffpkg', '.exfat')

    f2e_title_col = tk.Frame(f2e_chead, bg=COLORS['bg_2'])
    f2e_title_col.pack(side='left', fill='x', expand=True)
    tk.Label(f2e_title_col, text='ffpkg \u2192 exFAT',
             font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(fill='x')
    tk.Label(f2e_title_col,
             text='Pick an existing .ffpkg and a destination.',
             font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w'
             ).pack(fill='x', pady=(2, 0))

    tk.Frame(f2e_card, bg=COLORS['border_2'], height=1
             ).pack(fill='x')

    f2e_body = tk.Frame(f2e_card, bg=COLORS['bg_2'])
    f2e_body.pack(fill='x', padx=24, pady=(4, 18))

    def _f2e_browse_src():
        p = filedialog.askopenfilename(
            title='Select .ffpkg image',
            filetypes=[('ffpkg images', '*.ffpkg'),
                       ('All files', '*.*')])
        if p:
            f2e_src.set(p)

    def _f2e_browse_outdir():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            f2e_outdir.set(p)

    field_block(f2e_body, 'Source .ffpkg',
                 var=f2e_src, on_browse=_f2e_browse_src,
                 hint='the image to convert')
    field_block(f2e_body, 'Output folder',
                 var=f2e_outdir, on_browse=_f2e_browse_outdir,
                 hint='where the .exfat will be written')
    field_block(f2e_body, 'Output name',
                 var=f2e_name,
                 hint='auto-filled from source if blank')

    def _on_f2e_src(*_a):
        if f2e_src.get() and not f2e_name.get():
            base = os.path.splitext(os.path.basename(f2e_src.get()))[0]
            f2e_name.set(base + '.exfat')
        if f2e_src.get() and not f2e_outdir.get():
            f2e_outdir.set(os.path.dirname(f2e_src.get()))
        _update_hero(f2e_src.get().strip(), 'ffpkg', 'exFAT')
    f2e_src.trace_add('write', _on_f2e_src)

    f2e_action_row = tk.Frame(f2e_body, bg=COLORS['bg_2'])
    f2e_action_row.pack(fill='x', pady=(18, 0))

    f2e_btn = make_themed_button(
        f2e_action_row,
        text='Convert to exFAT',
        command=lambda: _do_ffpkg_to_exfat(),
        kind='success',
        icon='\u25b6',
        font_size=10, padx=18, pady=9)
    f2e_btn.pack(side='left')
    state['f2e_btn'] = f2e_btn

    tk.Label(f2e_action_row, textvariable=f2e_status_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w').pack(side='left', padx=(16, 0))

    f2e_pbar_wrap = tk.Frame(f2e_action_row, bg=COLORS['bg_2'])
    f2e_pbar_wrap.pack(side='right', fill='x', expand=True, padx=(16, 0))
    f2e_pbar = ttk.Progressbar(f2e_pbar_wrap, mode='indeterminate',
                                length=200)
    f2e_pbar.pack(fill='x')

    # Register f2e_btn back so the other set_busy locks it too.
    state['f2e_btn'] = f2e_btn

    def _set_busy_f2e(b, label=''):
        state['busy'] = b
        try:
            if b:
                f2e_pbar.start(10)
                f2e_status_var.set(label or 'Working...')
                f2e_btn.config(state='disabled', cursor='watch')
                # Lock the other card's button so the user can't fire
                # both concurrently.
                try:
                    convert_btn.config(state='disabled')
                except Exception:
                    pass
            else:
                f2e_pbar.stop()
                f2e_status_var.set(label or 'Idle.')
                f2e_btn.config(state='normal', cursor='hand2')
                try:
                    convert_btn.config(state='normal')
                except Exception:
                    pass
        except Exception:
            pass

    # ── ffpkg → exFAT worker ─────────────────────────────────────────
    def _do_ffpkg_to_exfat():
        if state['busy']:
            return
        src = f2e_src.get().strip()
        outdir = f2e_outdir.get().strip()
        name = f2e_name.get().strip()
        if not src or not os.path.isfile(src):
            messagebox.showerror('Source missing',
                'Pick a valid .ffpkg source image.')
            return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Output folder missing',
                'Pick an output folder.')
            return
        if not name:
            messagebox.showerror('Output name missing',
                'Set an output filename.')
            return
        if not name.lower().endswith('.exfat'):
            name = name + '.exfat'
        out_path = os.path.join(outdir, name)
        # Safety: never let the output path equal the source — we'd
        # delete the source as part of the overwrite step below.
        if os.path.abspath(out_path) == os.path.abspath(src):
            messagebox.showerror('Invalid output',
                'Output path is the same as the source. '
                'Pick a different name or folder.')
            return
        if os.path.exists(out_path):
            if not messagebox.askyesno('Overwrite',
                    out_path + '\n\nalready exists. Overwrite?'):
                return
            try:
                os.remove(out_path)
            except Exception as e:
                _log('Could not remove existing: ' + str(e))
                return

        # v3.0.0: OUTPUT LOG is never auto-opened (see e2f flow
        # above for the policy comment). The progress dialog
        # carries everything the user needs to see.

        # v3.0.0: rich progress dialog with ETA. Weights chosen from
        # observed runs — extract and copy each dominate roughly
        # half the wall time on real-world game-sized images.
        from ui.tab_ffpkg_edit import _RebuildProgress
        prog = _RebuildProgress(parent, 'Converting ffpkg → exFAT',
            weights={
                'extract':  (0,  50),
                'prep':     (50, 53),
                'copy':     (53, 98),
                'dismount': (98, 100),
            },
            initial_stage='extract')

        _set_busy_f2e(True, 'Locating tools...')

        def worker():
            import tempfile, shutil, ctypes as _ct, time as _time
            ufs2 = _get_ufs2tool_exe()
            if not ufs2:
                parent.after(0, prog.close)
                parent.after(0, lambda: _set_busy_f2e(False, 'Failed.'))
                parent.after(0, lambda: messagebox.showerror(
                    'UFS2Tool not available',
                    'UFS2Tool could not be extracted. Conversion '
                    'aborted.'))
                return

            osfmount = None
            try:
                osfmount = getattr(app, '_find_osfmount',
                                    lambda: None)()
            except Exception:
                osfmount = None
            if not osfmount or not os.path.isfile(osfmount):
                _log('OSFMount not found.')
                parent.after(0, prog.close)
                parent.after(0, lambda: _set_busy_f2e(False, 'Failed.'))
                parent.after(0, lambda: messagebox.showerror(
                    'OSFMount missing',
                    'OSFMount is required to convert ffpkg to exFAT.\n'
                    'Configure its path under Settings.'))
                return

            work_dir = tempfile.mkdtemp(prefix='ffpkg_to_exfat_')
            dump_dir = os.path.join(work_dir, 'dump')

            # Stop flag shared between worker and the extract-progress
            # poll thread defined below.
            extract_poll_stop = threading.Event()

            try:
                # ── Step 1: extract .ffpkg with UFS2Tool ──────────────
                parent.after(0, prog.set_stage, 'extract',
                    'Extracting .ffpkg...')
                _log('Extracting %s ...' % src)
                os.makedirs(dump_dir, exist_ok=True)

                # Background poll: UFS2Tool extract doesn't emit
                # progress, so estimate from bytes-on-disk vs the
                # source ffpkg size. Capped at 95% so we never claim
                # done before UFS2Tool actually returns.
                try:
                    src_size = os.path.getsize(src)
                except Exception:
                    src_size = 0

                def _poll_extract():
                    while not extract_poll_stop.is_set():
                        try:
                            seen = 0
                            for r, _ds, fs in os.walk(dump_dir):
                                for f in fs:
                                    try:
                                        seen += os.path.getsize(
                                            os.path.join(r, f))
                                    except Exception:
                                        pass
                            if src_size > 0:
                                pct = min(95.0,
                                    100.0 * seen / src_size)
                            else:
                                pct = 0.0
                            detail = ('%.2f GB extracted'
                                      % (seen / 1024**3))
                            parent.after(0,
                                prog.set_stage_progress, pct, detail)
                        except Exception:
                            pass
                        extract_poll_stop.wait(0.7)

                poll_thread = threading.Thread(target=_poll_extract,
                                                daemon=True)
                poll_thread.start()

                rc = _run([ufs2, 'extract', src, dump_dir],
                          'UFS2Tool extract')
                if rc != 0:
                    # Try the explicit '/' form for older UFS2Tool.
                    rc = _run([ufs2, 'extract', src, dump_dir, '/'],
                              'UFS2Tool extract (retry with /)')

                extract_poll_stop.set()
                if rc != 0:
                    raise RuntimeError(
                        'UFS2Tool extract failed (rc=%d)' % rc)

                # Count files + total size for the next step.
                total_bytes = 0
                file_count  = 0
                for r, ds, fs in os.walk(dump_dir):
                    for f in fs:
                        try:
                            total_bytes += os.path.getsize(
                                os.path.join(r, f))
                            file_count += 1
                        except Exception:
                            pass
                if file_count == 0:
                    raise RuntimeError(
                        'Extraction produced no files. '
                        'Source image may be corrupt.')
                _log('Extracted %d files, %.2f GB total.'
                     % (file_count, total_bytes / 1024**3))
                parent.after(0, prog.set_stage_progress, 100.0,
                    '%d files, %.2f GB extracted.'
                    % (file_count, total_bytes / 1024**3))

                # ── Step 2: prep (allocate + mount + format) ─────────
                parent.after(0, prog.set_stage, 'prep',
                    'Preparing exFAT image...')

                # Size = extracted bytes × 1.10, rounded up to the
                # next 64 MB. The 10% headroom covers exFAT cluster
                # waste and directory metadata; 64 MB alignment keeps
                # OSFMount happy (it dislikes oddly-sized images).
                target_size = int(total_bytes * 1.10)
                ALIGN = 64 * 1024 * 1024
                target_size = ((target_size + ALIGN - 1) // ALIGN) * ALIGN
                if target_size < ALIGN:
                    target_size = ALIGN

                parent.after(0, prog.set_stage_progress, 10.0,
                    'Allocating image (%.2f GB)...'
                    % (target_size / 1024**3))
                _log('Allocating blank .exfat at %s (%.2f GB)...'
                     % (out_path, target_size / 1024**3))
                try:
                    with open(out_path, 'wb') as fh:
                        fh.seek(target_size - 1)
                        fh.write(b'\0')
                except Exception as e:
                    raise RuntimeError(
                        'Failed to allocate output file: ' + str(e))

                # Pick a free drive letter
                used_mask = _ct.windll.kernel32.GetLogicalDrives()
                mount_letter = None
                for code in range(ord('G'), ord('Z') + 1):
                    if not (used_mask & (1 << (code - ord('A')))):
                        mount_letter = chr(code) + ':'
                        break
                if not mount_letter:
                    raise RuntimeError('No free drive letter for mount.')

                # Mount the blank file writable
                parent.after(0, prog.set_stage_progress, 30.0,
                    'Mounting at ' + mount_letter)
                _log('Mounting %s at %s ...' % (out_path, mount_letter))
                rc = _run([osfmount, '-a', '-t', 'file',
                           '-f', out_path,
                           '-m', mount_letter, '-o', 'rw'],
                          'OSFMount attach')
                if rc != 0:
                    raise RuntimeError(
                        'Mount failed (rc=%d)' % rc)

                # Wait for the drive to appear.
                for _ in range(20):
                    if os.path.exists(mount_letter + '\\'):
                        break
                    _time.sleep(0.5)

                try:
                    # Format as exFAT — format.com prompts even with
                    # /Y, so pipe newlines via stdin to suppress hang.
                    parent.after(0, prog.set_stage_progress, 60.0,
                        'Formatting %s as exFAT...' % mount_letter)
                    _log('Formatting %s as exFAT...' % mount_letter)
                    fmt_cmd = ['cmd.exe', '/c', 'format',
                               mount_letter, '/FS:exFAT', '/Q', '/Y',
                               '/V:']
                    _log('Running: ' + ' '.join(fmt_cmd))
                    try:
                        CREATE_NO_WINDOW = 0x08000000
                        kwargs = ({'creationflags': CREATE_NO_WINDOW}
                                  if os.name == 'nt' else {})
                        fp = subprocess.Popen(fmt_cmd,
                            stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            text=True, errors='replace', **kwargs)
                        try:
                            fp.stdin.write('\n\n\n')
                            fp.stdin.flush()
                            fp.stdin.close()
                        except Exception:
                            pass
                        for ln in fp.stdout:
                            _log(ln.rstrip())
                        fp.wait(timeout=180)
                        rc = fp.returncode
                    except subprocess.TimeoutExpired:
                        try:
                            fp.kill()
                        except Exception:
                            pass
                        raise RuntimeError(
                            'format command timed out after 3 minutes.')
                    if rc != 0:
                        raise RuntimeError(
                            'format /FS:exFAT failed (rc=%d)' % rc)

                    _time.sleep(2)
                    if not os.path.exists(mount_letter + '\\'):
                        raise RuntimeError(
                            'Mounted drive disappeared after format.')
                    parent.after(0, prog.set_stage_progress, 100.0,
                        'Formatted, ready to copy.')

                    # ── Step 3: robocopy ─────────────────────────────
                    parent.after(0, prog.set_stage, 'copy',
                        'Copying %d files...' % file_count)
                    _log('Robocopying dump → %s ...' % mount_letter)

                    # Robocopy prints "    New File    <size>    <name>"
                    # per file. Count those to drive progress against
                    # the pre-scanned total.
                    files_copied = [0]
                    new_file_re = re.compile(
                        r'^\s*(?:New File|Newer)\b', re.IGNORECASE)
                    def _robo_cb(line):
                        if new_file_re.match(line):
                            files_copied[0] += 1
                            if file_count > 0:
                                pct = (100.0
                                       * files_copied[0] / file_count)
                            else:
                                pct = 0.0
                            detail = ('%d / %d files'
                                      % (files_copied[0], file_count))
                            parent.after(0,
                                prog.set_stage_progress, pct, detail)

                    robo_cmd = [
                        'robocopy.exe',
                        dump_dir, mount_letter + '\\',
                        '/E', '/COPY:DAT', '/DCOPY:DAT',
                        '/R:1', '/W:1', '/NP', '/ETA',
                    ]
                    rc = _run(robo_cmd, 'robocopy',
                              progress_cb=_robo_cb)
                    if rc >= 8:
                        raise RuntimeError(
                            'robocopy failed (rc=%d)' % rc)
                    _log('Copy complete.')
                    parent.after(0, prog.set_stage_progress, 100.0,
                        'Copied %d files.' % files_copied[0])
                finally:
                    # ── Step 4: dismount ─────────────────────────────
                    parent.after(0, prog.set_stage, 'dismount',
                        'Unmounting ' + mount_letter)
                    _log('Unmounting %s ...' % mount_letter)
                    try:
                        letter = mount_letter.rstrip(':\\')
                        if hasattr(app, '_dismount_drive_robust'):
                            app._dismount_drive_robust(letter,
                                max_wait_seconds=20)
                        else:
                            _run([osfmount, '-d', '-m', mount_letter])
                    except Exception as e:
                        _log('Dismount error: ' + str(e))
                    parent.after(0, prog.set_stage_progress, 100.0)

                parent.after(0, prog.close)
                parent.after(0, lambda:
                    _set_busy_f2e(False, 'Done \u2713'))
                parent.after(0, lambda: messagebox.showinfo(
                    'Convert complete',
                    'Wrote:\n' + out_path))
                def _nudge_cv2():
                    try:
                        from ui.release_notes import note_successful_operation
                        note_successful_operation(app, 'Convert')
                    except Exception:
                        pass
                parent.after(700, _nudge_cv2)
            except Exception as e:
                extract_poll_stop.set()
                _log('Convert failed: ' + str(e))
                parent.after(0, prog.close)
                parent.after(0, lambda e=e:
                    _set_busy_f2e(False, 'Failed.'))
                parent.after(0, lambda e=e: messagebox.showerror(
                    'ffpkg → exFAT failed', str(e)))
                # Best-effort cleanup of half-finished output file.
                try:
                    if os.path.exists(out_path):
                        os.remove(out_path)
                except Exception:
                    pass
            finally:
                extract_poll_stop.set()
                # Always clean up the temp dump dir.
                try:
                    shutil.rmtree(work_dir, ignore_errors=True)
                except Exception:
                    pass

        threading.Thread(target=worker, daemon=True).start()


