"""ui/tab_convert_pfs_cards.py — exFAT/ffpkg → .ffpfsc cards for the Convert tab.

Adds two cards below the existing exFAT↔ffpkg cards:

    .exfat → .ffpfsc   (existing exFAT image → compressed PFS)
    .ffpkg → .ffpfsc   (existing ffpkg image → compressed PFS)

Dump folder → .ffpfsc lives on the dedicated PFS tab.

Uses mkpfs in-process via ui.mkpfs_runner so it works in the frozen exe.

Entry point:
    from ui.tab_convert_pfs_cards import attach_pfs_cards
    attach_pfs_cards(inner, app, state, _log, _run, parent)
"""

import os
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from tkinter_theme import COLORS, FONTS
from ui.shared.page_head import make_themed_button, field_block


def _section_divider(parent):
    wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    wrap.pack(fill='x', padx=24, pady=(8, 4))
    tk.Frame(wrap, bg=COLORS['border_2'], height=1).pack(fill='x')
    tk.Label(wrap, text='\u2192  .FFPFSC  (MicroMount compressed PFS images)',
             font=FONTS['eyebrow'],
             bg=COLORS['bg_1'], fg=COLORS['fg_5'],
             anchor='w').pack(fill='x', pady=(6, 0))


def _pfs_card(parent, title, subtitle, source_label, source_ext, source_filetypes):
    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    card.pack(fill='x', padx=24, pady=(0, 14))

    chead = tk.Frame(card, bg=COLORS['bg_2'])
    chead.pack(fill='x', padx=24, pady=(18, 14))
    ico = tk.Label(chead, text='\u2192',
                   font=(FONTS['h2'][0], 13),
                   bg=COLORS['accent_08'], fg=COLORS['teal'],
                   width=2, padx=4, pady=2)
    ico.pack(side='left', padx=(0, 12))
    title_col = tk.Frame(chead, bg=COLORS['bg_2'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=title,
             font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(fill='x')
    tk.Label(title_col, text=subtitle,
             font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w').pack(fill='x', pady=(2, 0))

    tk.Frame(card, bg=COLORS['border_2'], height=1).pack(fill='x')

    body = tk.Frame(card, bg=COLORS['bg_2'])
    body.pack(fill='x', padx=24, pady=(4, 18))

    src_var      = tk.StringVar()
    outdir_var   = tk.StringVar()
    name_var     = tk.StringVar()
    status_var   = tk.StringVar(value='Idle.')
    compress_var = tk.BooleanVar(value=True)
    verify_var   = tk.BooleanVar(value=False)

    def _browse_src():
        p = filedialog.askopenfilename(title='Select ' + source_label,
                                        filetypes=source_filetypes)
        if p:
            src_var.set(p)

    def _browse_outdir():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            outdir_var.set(p)

    field_block(body, source_label, var=src_var, on_browse=_browse_src,
                hint='the %s image to convert' % source_ext)
    field_block(body, 'Output folder', var=outdir_var, on_browse=_browse_outdir,
                hint='where the .ffpfsc will be written')
    field_block(body, 'Output name', var=name_var,
                hint='auto-filled from source if blank')

    def _on_src(*_):
        p = src_var.get()
        if not p:
            return
        if not outdir_var.get():
            outdir_var.set(os.path.dirname(p))
        if not name_var.get():
            name_var.set(os.path.splitext(os.path.basename(p))[0] + '.ffpfsc')
    src_var.trace_add('write', _on_src)

    opts_row = tk.Frame(body, bg=COLORS['bg_2'])
    opts_row.pack(fill='x', pady=(12, 0))
    tk.Label(opts_row, text='Options:', font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left')

    def _cb_style(cb, var):
        def _upd(*_):
            cb.config(fg=COLORS['teal'] if var.get() else COLORS['fg_4'])
        var.trace_add('write', _upd); _upd()

    version_ps5_var = tk.BooleanVar(value=True)

    for text, var in (('--compress', compress_var), ('--verify', verify_var),
                      ('--version PS5', version_ps5_var)):
        cb = tk.Checkbutton(opts_row, text=text, variable=var,
                             font=FONTS['mono_sm'],
                             bg=COLORS['bg_2'], fg=COLORS['teal'],
                             selectcolor=COLORS['bg_3'],
                             activebackground=COLORS['bg_2'],
                             activeforeground=COLORS['teal'],
                             bd=0, padx=8, cursor='hand2')
        cb.pack(side='left', padx=(10 if text == '--compress' else 0, 0))
        _cb_style(cb, var)

    tk.Label(opts_row, text='  \u2756  recommended for MicroMount',
             font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_5']).pack(side='left')

    action_row = tk.Frame(body, bg=COLORS['bg_2'])
    action_row.pack(fill='x', pady=(18, 0))

    btn = make_themed_button(action_row, text='Convert to .ffpfsc',
                              command=lambda: None,
                              kind='success', icon='\u25b6',
                              font_size=10, padx=18, pady=9)
    btn.pack(side='left')
    tk.Label(action_row, textvariable=status_var,
             font=FONTS['mono_sm'], bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w').pack(side='left', padx=(16, 0))
    pbar_wrap = tk.Frame(action_row, bg=COLORS['bg_2'])
    pbar_wrap.pack(side='right', fill='x', expand=True, padx=(16, 0))
    pbar = ttk.Progressbar(pbar_wrap, mode='indeterminate', length=200)
    pbar.pack(fill='x')

    return {'src': src_var, 'outdir': outdir_var, 'name': name_var,
            'status': status_var, 'compress': compress_var,
            'verify': verify_var, 'version_ps5': version_ps5_var,
            'pbar': pbar, 'btn': btn, 'card': card}


def attach_pfs_cards(inner, app, state, _log, _run, parent):
    _section_divider(inner)

    e2pfs = _pfs_card(inner,
        title='.exfat \u2192 .ffpfsc',
        subtitle='Convert an existing exFAT image to a MicroMount-ready compressed PFS image.',
        source_label='Source .exfat',
        source_ext='.exfat',
        source_filetypes=[('exFAT images', '*.exfat'), ('All files', '*.*')])

    f2pfs = _pfs_card(inner,
        title='.ffpkg \u2192 .ffpfsc',
        subtitle='Convert an existing ffpkg image to a MicroMount-ready compressed PFS image.',
        source_label='Source .ffpkg',
        source_ext='.ffpkg',
        source_filetypes=[('ffpkg images', '*.ffpkg'), ('All files', '*.*')])

    all_pfs_btns = [e2pfs['btn'], f2pfs['btn']]

    def _make_worker(card_vars, pack_mode):
        """pack_mode: 'file' for exfat/ffpkg (single image file input)."""
        def _run_conversion():
            if state.get('busy'):
                return

            src    = card_vars['src'].get().strip()
            outdir = card_vars['outdir'].get().strip()
            name   = card_vars['name'].get().strip()

            if not src or not os.path.isfile(src):
                messagebox.showerror('Source missing', 'Pick a valid source image file.')
                return
            if not outdir or not os.path.isdir(outdir):
                messagebox.showerror('Output folder missing', 'Pick an output folder.')
                return
            if not name:
                messagebox.showerror('Output name missing', 'Set an output filename.')
                return
            if not name.lower().endswith('.ffpfsc'):
                name += '.ffpfsc'
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

            state['busy'] = True
            for b in all_pfs_btns:
                try: b.config(state='disabled')
                except Exception: pass

            card_vars['pbar'].start(10)
            card_vars['status'].set('Converting\u2026')

            def worker():
                try:
                    from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                    if not mkpfs_version():
                        parent.after(0, lambda: card_vars['status'].set('mkpfs not available.'))
                        parent.after(0, lambda: messagebox.showerror(
                            'mkpfs not available',
                            'mkpfs is not bundled in this build.'))
                        return

                    argv = ['pack', pack_mode]
                    if card_vars['compress'].get():
                        argv.append('--compress')
                    if card_vars['verify'].get():
                        argv.append('--verify')
                    if card_vars.get('version_ps5', tk.BooleanVar(value=False)).get():
                        argv += ['--version', 'PS5']
                    # Force single-process compression when frozen.
                    import sys as _sys
                    if getattr(_sys, 'frozen', False):
                        argv += ['--cpu-count', '1']
                    argv += [src, out_path]

                    _log('mkpfs ' + ' '.join(argv))

                    def _on_progress(phase, pct, total, detail):
                        parent.after(0, lambda d=detail:
                            card_vars['status'].set(d[:80] if d.strip()
                                                    else card_vars['status'].get()))

                    rc = run_mkpfs(argv, log_cb=_log, progress_cb=_on_progress)

                    if rc == 0:
                        size_mb = os.path.getsize(out_path) / 1024 ** 2
                        parent.after(0, lambda: card_vars['status'].set(
                            'Done \u2713  (%.1f MB)' % size_mb))
                        parent.after(0, lambda: messagebox.showinfo(
                            'Convert complete', 'Wrote:\n' + out_path))
                    else:
                        parent.after(0, lambda: card_vars['status'].set(
                            'mkpfs failed (rc=%d).' % rc))
                        parent.after(0, lambda: messagebox.showerror(
                            'mkpfs failed',
                            'mkpfs exited with code %d.\n\n'
                            'Check the OUTPUT LOG for details.' % rc))
                        try:
                            if os.path.exists(out_path):
                                os.remove(out_path)
                        except Exception:
                            pass

                except Exception as e:
                    _log('PFS convert error: ' + str(e))
                    parent.after(0, lambda e=e: card_vars['status'].set(
                        'Error: ' + str(e)[:60]))
                    parent.after(0, lambda e=e: messagebox.showerror(
                        'Conversion error', str(e)))
                finally:
                    state['busy'] = False
                    parent.after(0, card_vars['pbar'].stop)
                    for b in all_pfs_btns:
                        try: parent.after(0, lambda b=b: b.config(state='normal'))
                        except Exception: pass

            threading.Thread(target=worker, daemon=True).start()
        return _run_conversion

    # exfat is a single image file → pack file mode
    # ffpkg is also a single image file → pack file mode
    e2pfs['btn'].config(command=_make_worker(e2pfs, 'file'))
    f2pfs['btn'].config(command=_make_worker(f2pfs, 'file'))

    # mkpfs badge
    badge_var = tk.StringVar(value='Checking mkpfs\u2026')
    badge_row = tk.Frame(inner, bg=COLORS['bg_1'])
    badge_row.pack(fill='x', padx=24, pady=(0, 18))
    badge_lbl = tk.Label(badge_row, textvariable=badge_var,
                          font=FONTS['mono_sm'],
                          bg=COLORS['bg_1'], fg=COLORS['fg_4'], anchor='w')
    badge_lbl.pack(side='left')

    def _check_badge():
        from ui.mkpfs_runner import mkpfs_version
        ver = mkpfs_version()
        if ver:
            parent.after(0, lambda: (
                badge_var.set('\u2713  mkpfs  ' + ver),
                badge_lbl.config(fg=COLORS['teal'])))
        else:
            parent.after(0, lambda: (
                badge_var.set('\u2717  mkpfs not available'),
                badge_lbl.config(fg=COLORS['danger'])))

    threading.Thread(target=_check_badge, daemon=True).start()
