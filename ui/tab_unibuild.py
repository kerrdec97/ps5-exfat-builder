"""Unified Build tab — premium dashboard layout.

One Build tab that targets exFAT, ffpkg, or PFS from a single game dump,
with a mixed queue (jobs of different output types build in order).  PFS
jobs build an intermediate exFAT (default) or ffpkg image first, then convert
it to .ffpfsc — reusing the existing pipeline.

This module is UI/presentation only — it drives the existing exFAT / ffpkg /
pipeline builders and does not change any build, queue, threading or settings
logic.
"""
import os
import time
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from tkinter_theme import COLORS, FONTS
from ui.shared.page_head import (
    make_themed_button, info_banner, page_head, field_block)

# extension + label per output type
_TYPE_EXT = {'exfat': '.exfat', 'ffpkg': '.ffpkg', 'pfs': '.ffpfsc'}
_TYPE_LABEL = {'exfat': 'exFAT', 'ffpkg': 'ffpkg', 'pfs': 'PFS'}

# status -> (badge text, colour key)
_STATUS_BADGE = {
    'queued':  ('QUEUED',   'fg_4'),
    'running': ('RUNNING',  'accent'),
    'done':    ('COMPLETE', 'success'),
    'failed':  ('FAILED',   'danger'),
    'skipped': ('SKIPPED',  'fg_5'),
}

_DARK_TEXT = '#001a05'   # readable text on the green/teal pills
_SEL_BG    = '#242c3a'   # bg_3 + ~12% teal — selected-tile fill (subtle, no glow)


def _dash_card(parent, title, subtitle=None,
               head_pady=(16, 2), body_pady=(10, 18)):
    """A dashboard section card with a coloured title + optional subtitle.
    Returns (card, body). head_pady/body_pady allow per-card compaction."""
    card = tk.Frame(parent, bg=COLORS['bg_2'], highlightthickness=1,
                    highlightbackground=COLORS['border_2'])
    head = tk.Frame(card, bg=COLORS['bg_2'])
    head.pack(fill='x', padx=20, pady=head_pady)
    tk.Label(head, text=title, font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['accent'], anchor='w').pack(anchor='w')
    if subtitle:
        tk.Label(head, text=subtitle, font=FONTS['meta'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w').pack(
                     anchor='w', pady=(2, 0))
    body = tk.Frame(card, bg=COLORS['bg_2'])
    body.pack(fill='both', expand=True, padx=20, pady=body_pady)
    return card, body


def _hero_stat(parent, label_text, var):
    box = tk.Frame(parent, bg=COLORS['bg_3'], highlightthickness=1,
                   highlightbackground=COLORS['border_3'])
    tk.Label(box, text=label_text, font=FONTS['eyebrow'],
             bg=COLORS['bg_3'], fg=COLORS['fg_5'], anchor='w').pack(
                 anchor='w', padx=10, pady=(7, 0))
    val = tk.Label(box, textvariable=var, font=(FONTS['mono_sm'][0], 11, 'bold'),
                   bg=COLORS['bg_3'], fg=COLORS['fg_1'], anchor='w')
    val.pack(anchor='w', padx=10, pady=(0, 7))
    return box, val


def _strip_cell(parent, label_text, var):
    """Eyebrow + value cell for the build-stats strip. Returns (cell, value
    label) so callers can recolour the value (e.g. low-space warning)."""
    cell = tk.Frame(parent, bg=COLORS['bg_3'])
    tk.Label(cell, text=label_text, font=FONTS['eyebrow'],
             bg=COLORS['bg_3'], fg=COLORS['fg_5'], anchor='w').pack(
                 anchor='w', padx=14, pady=(8, 0))
    val = tk.Label(cell, textvariable=var, font=(FONTS['mono_sm'][0], 11, 'bold'),
                   bg=COLORS['bg_3'], fg=COLORS['teal'], anchor='w')
    val.pack(anchor='w', padx=14, pady=(0, 8))
    return cell, val


def _toast(app, title, line1='', line2='', ok=True):
    """Lightweight bottom-right auto-dismissing toast (no external libs)."""
    try:
        t = tk.Toplevel(app)
        t.overrideredirect(True)
        try:
            t.attributes('-topmost', True)
        except Exception:
            pass
        accent = COLORS['success'] if ok else COLORS['warn']
        frame = tk.Frame(t, bg=COLORS['bg_3'], highlightthickness=1,
                         highlightbackground=accent)
        frame.pack(fill='both', expand=True)
        pad = tk.Frame(frame, bg=COLORS['bg_3'])
        pad.pack(fill='both', expand=True, padx=16, pady=13)
        tk.Label(pad, text=('\u2713' if ok else '\u26a0'),
                 font=(FONTS['h2'][0], 15, 'bold'), bg=COLORS['bg_3'],
                 fg=accent).pack(side='left', padx=(0, 13))
        col = tk.Frame(pad, bg=COLORS['bg_3'])
        col.pack(side='left')
        tk.Label(col, text=title, font=(FONTS['h3'][0], 11, 'bold'),
                 bg=COLORS['bg_3'], fg=COLORS['fg_0'], anchor='w').pack(anchor='w')
        if line1:
            tk.Label(col, text=line1, font=FONTS['meta'], bg=COLORS['bg_3'],
                     fg=COLORS['fg_3'], anchor='w').pack(anchor='w')
        if line2:
            tk.Label(col, text=line2, font=FONTS['meta'], bg=COLORS['bg_3'],
                     fg=COLORS['fg_4'], anchor='w').pack(anchor='w')
        app.update_idletasks()
        t.update_idletasks()
        tw, th = t.winfo_width(), t.winfo_height()
        ax, ay = app.winfo_rootx(), app.winfo_rooty()
        aw, ah = app.winfo_width(), app.winfo_height()
        t.geometry('+%d+%d' % (ax + aw - tw - 26, ay + ah - th - 30))
        t.after(5000, t.destroy)
        for w in (frame, pad, col):
            w.bind('<Button-1>', lambda e: t.destroy())
    except Exception:
        pass


def build_unibuild_tab(parent, app):
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

    # ── State ────────────────────────────────────────────────────────
    src_var    = tk.StringVar()
    outdir_var = tk.StringVar()
    name_var   = tk.StringVar()
    out_type   = tk.StringVar(value='exfat')   # exfat | ffpkg | pfs
    pfs_via    = tk.StringVar(value='exfat')   # exfat | ffpkg  (pfs only)
    pfs_src_mode = tk.StringVar(value='folder')  # folder | image (pfs only)
    name_preset = tk.StringVar(value='ppsa_title_ver')
    tempdir_var = tk.StringVar(value=str(app._settings.get('temp_dir') or ''))

    det_title_var = tk.StringVar(value='No game dump selected')
    det_id_var    = tk.StringVar(value='')
    hero_ver_var  = tk.StringVar(value='\u2014')
    hero_size_var = tk.StringVar(value='\u2014')
    hero_free_var = tk.StringVar(value='\u2014')

    stat_jobs_var    = tk.StringVar(value='No queued jobs')
    stat_estsize_var = tk.StringVar(value='\u2014')
    stat_needed_var  = tk.StringVar(value='\u2014')
    stat_space_var   = tk.StringVar(value='\u2014')
    stat_comp_var    = tk.StringVar(value='')

    sum_title_var = tk.StringVar(value='BUILD COMPLETE')
    sum_jobs_var  = tk.StringVar(value='')

    uq_status   = tk.StringVar(value='')
    uq_step_var = tk.StringVar(value='')
    uq_pct_var  = tk.StringVar(value='')
    uq_eta_var  = tk.StringVar(value='')
    uq_count    = tk.StringVar(value='')

    detected  = {}
    cover_ref = {'img': None}
    tile_refs = {}
    queue     = []
    uq_state  = {'sel': {}, 'running': False}

    def _browse_src():
        if out_type.get() == 'pfs' and pfs_src_mode.get() == 'image':
            p = filedialog.askopenfilename(
                title='Select an existing .exfat / .ffpkg image',
                filetypes=[('Game images', '*.exfat *.ffpkg'),
                           ('exFAT images', '*.exfat'),
                           ('ffpkg images', '*.ffpkg'),
                           ('All files', '*.*')])
        else:
            p = filedialog.askdirectory(title='Select game dump folder')
        if p:
            src_var.set(p)

    def _browse_out():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            outdir_var.set(p)

    def _browse_temp():
        p = filedialog.askdirectory(title='Select temp / spool folder')
        if p:
            tempdir_var.set(p)

    # ══════════════════════════════════════════════════════════════════
    # ROW 1 — game hero card  +  output-format card
    # ══════════════════════════════════════════════════════════════════
    row1 = tk.Frame(inner, bg=COLORS['bg_1'])
    row1.pack(fill='x', padx=24, pady=(18, 12))
    row1.columnconfigure(0, weight=5, uniform='r1')
    row1.columnconfigure(1, weight=6, uniform='r1')

    # ── Hero card ──
    hero = tk.Frame(row1, bg=COLORS['bg_2'], highlightthickness=1,
                    highlightbackground=COLORS['border_2'])
    hero.grid(row=0, column=0, sticky='nsew', padx=(0, 6))
    hero_in = tk.Frame(hero, bg=COLORS['bg_2'])
    hero_in.pack(fill='both', expand=True, padx=16, pady=16)

    cover_lbl = tk.Label(hero_in, text='\U0001f3ae', font=('Segoe UI', 56),
                         bg=COLORS['bg_4'], fg=COLORS['fg_5'], width=4, height=4)
    cover_lbl.pack(side='left', padx=(0, 16))

    hcol = tk.Frame(hero_in, bg=COLORS['bg_2'])
    hcol.pack(side='left', fill='both', expand=True)
    tk.Label(hcol, textvariable=det_title_var, font=(FONTS['h2'][0], 16, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w',
             justify='left', wraplength=300).pack(fill='x')
    tk.Label(hcol, textvariable=det_id_var, font=(FONTS['mono_sm'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['accent'], anchor='w').pack(
                 fill='x', pady=(2, 8))

    stat_row = tk.Frame(hcol, bg=COLORS['bg_2'])
    stat_row.pack(fill='x')
    for c in range(3):
        stat_row.columnconfigure(c, weight=1, uniform='hs')
    b1, _ = _hero_stat(stat_row, '\U0001f9e9  VERSION', hero_ver_var)
    b1.grid(row=0, column=0, sticky='ew', padx=(0, 6))
    b2, _ = _hero_stat(stat_row, '\U0001f4be  DUMP SIZE', hero_size_var)
    b2.grid(row=0, column=1, sticky='ew', padx=6)
    b3, free_val = _hero_stat(stat_row, '\U0001f5c4  OUTPUT FREE', hero_free_var)
    b3.grid(row=0, column=2, sticky='ew', padx=(6, 0))

    hero_bottom = tk.Frame(hcol, bg=COLORS['bg_2'])
    hero_bottom.pack(fill='x', pady=(10, 0))
    ready_badge = tk.Label(hero_bottom, text='\u2713 READY TO BUILD',
                           font=(FONTS['mono_sm'][0], 9, 'bold'),
                           bg=COLORS['success'], fg=_DARK_TEXT, padx=10, pady=4)
    make_themed_button(hero_bottom, text='Browse dump',
                       command=lambda: _browse_src(), kind='accent',
                       icon='\U0001f4c1', font_size=9, padx=12,
                       pady=6).pack(side='right')

    # ── Output-format card ──
    fmt_card, fmt_body = _dash_card(row1, '1.  Choose output format',
                                    'Pick a format below and add the dump to the queue')
    fmt_card.grid(row=0, column=1, sticky='nsew', padx=(6, 0))

    fmt_row = tk.Frame(fmt_body, bg=COLORS['bg_2'])
    fmt_row.pack(fill='x')
    for c in range(3):
        fmt_row.columnconfigure(c, weight=1, uniform='fmt')

    def _restyle_tiles():
        for k, ref in tile_refs.items():
            sel = (out_type.get() == k)
            bg = _SEL_BG if sel else COLORS['bg_3']
            ref['card'].config(
                highlightbackground=COLORS['teal'] if sel else COLORS['border_2'],
                highlightthickness=2 if sel else 1, bg=bg)
            for w in ref['labels']:
                w.config(bg=bg)
            ref['labels'][0].config(fg=COLORS['teal'])
            ref['labels'][1].config(fg=COLORS['teal'] if sel else COLORS['fg_1'])
            if sel:
                ref['check'].place(relx=1.0, x=-8, y=8, anchor='ne')
            else:
                ref['check'].place_forget()

    def _pick_type(k):
        out_type.set(k)
        if k != 'pfs':
            pfs_src_mode.set('folder')
        _restyle_tiles()
        _refresh_via()
        _refresh_size_row()
        _refresh_strip()
        _gen_name()
        _update_space()

    def _make_type_card(col, key, icon, title, sub, recommended=False):
        card = tk.Frame(fmt_row, bg=COLORS['bg_3'], highlightthickness=1,
                        highlightbackground=COLORS['border_2'], cursor='hand2')
        card.grid(row=0, column=col, sticky='nsew', padx=(0 if col == 0 else 8, 0))
        labels = []
        ic = tk.Label(card, text=icon, font=(FONTS['h2'][0], 19),
                      bg=COLORS['bg_3'], fg=COLORS['teal'], anchor='w')
        ic.pack(anchor='w', padx=14, pady=(14, 4))
        labels.append(ic)
        nm = tk.Label(card, text=title, font=(FONTS['h3'][0], 12, 'bold'),
                      bg=COLORS['bg_3'], fg=COLORS['fg_1'], anchor='w')
        nm.pack(fill='x', padx=14)
        labels.append(nm)
        sbl = tk.Label(card, text=sub, font=FONTS['meta'], bg=COLORS['bg_3'],
                       fg=COLORS['fg_4'], anchor='w')
        sbl.pack(fill='x', padx=14, pady=(2, 0))
        labels.append(sbl)
        if recommended:
            tk.Label(card, text='RECOMMENDED',
                     font=(FONTS['mono_sm'][0], 8, 'bold'),
                     bg=COLORS['success'], fg=_DARK_TEXT, padx=6, pady=1).pack(
                         anchor='w', padx=14, pady=(8, 14))
        else:
            sp = tk.Label(card, text='', bg=COLORS['bg_3'])
            sp.pack(pady=(4, 12))
            labels.append(sp)   # restyled with the card so selection fills evenly
        check = tk.Label(card, text='\u2713',
                         font=(FONTS['mono_sm'][0], 10, 'bold'),
                         bg=COLORS['teal'], fg=_DARK_TEXT, width=2)
        for w in [card] + labels:
            w.bind('<Button-1>', lambda e, kk=key: _pick_type(kk))
        tile_refs[key] = {'card': card, 'labels': labels, 'check': check}

    _make_type_card(0, 'exfat', '\U0001f528', 'exFAT', 'Mountable image',
                    recommended=True)
    _make_type_card(1, 'ffpkg', '\U0001f4e6', 'ffpkg', 'UFS package')
    _make_type_card(2, 'pfs', '\U0001f5dc', 'PFS', 'Compressed container')

    via_frame = tk.Frame(fmt_body, bg=COLORS['bg_2'])
    tk.Label(via_frame, text='PFS \u2014 build intermediate via:',
             font=FONTS['mono_sm'], bg=COLORS['bg_2'],
             fg=COLORS['fg_3']).pack(side='left', padx=(0, 10))
    for val, lab in (('exfat', 'exFAT (default)'), ('ffpkg', 'ffpkg')):
        tk.Radiobutton(via_frame, text=lab, value=val, variable=pfs_via,
                       command=lambda: (_refresh_strip(), _update_space()),
                       font=FONTS['mono_sm'], bg=COLORS['bg_2'],
                       fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'],
                       activebackground=COLORS['bg_2'],
                       activeforeground=COLORS['teal'],
                       highlightthickness=0, bd=0).pack(side='left', padx=(0, 12))

    # PFS source mode: build from a game dump folder (default) or pack an
    # existing .exfat / .ffpkg straight into the .ffpfsc (skips the
    # intermediate build entirely — same pack step, no extraction).
    pfs_mode_frame = tk.Frame(fmt_body, bg=COLORS['bg_2'])
    tk.Label(pfs_mode_frame, text='PFS — source:',
             font=FONTS['mono_sm'], bg=COLORS['bg_2'],
             fg=COLORS['fg_3']).pack(side='left', padx=(0, 10))
    for _val, _lab in (('folder', 'Game dump folder'),
                       ('image', 'Existing .exfat / .ffpkg')):
        tk.Radiobutton(pfs_mode_frame, text=_lab, value=_val,
                       variable=pfs_src_mode,
                       command=lambda: _on_pfs_mode_change(),
                       font=FONTS['mono_sm'], bg=COLORS['bg_2'],
                       fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'],
                       activebackground=COLORS['bg_2'],
                       activeforeground=COLORS['teal'],
                       highlightthickness=0, bd=0).pack(side='left', padx=(0, 12))

    def _refresh_via():
        # The "build intermediate via" choice only applies when building a
        # PFS from a dump folder. Packing an existing image needs no
        # intermediate, so it is hidden in image mode.
        if out_type.get() == 'pfs':
            pfs_mode_frame.pack(anchor='w', pady=(12, 0))
        else:
            pfs_mode_frame.pack_forget()
        if out_type.get() == 'pfs' and pfs_src_mode.get() == 'folder':
            via_frame.pack(anchor='w', pady=(8, 0))
        else:
            via_frame.pack_forget()

    def _on_pfs_mode_change():
        # Switching source mode clears the current selection so a folder
        # path isn't mistaken for an image (or vice versa).
        src_var.set('')
        det_title_var.set('—')
        det_id_var.set('—')
        hero_ver_var.set('—')
        cover_ref['img'] = None
        try:
            cover_lbl.config(image='',
                             text='🗜' if pfs_src_mode.get() == 'image'
                             else '🎮',
                             font=('Segoe UI', 56), width=4, height=4)
        except Exception:
            pass
        _refresh_via()
        _refresh_strip()
        _refresh_ready()
        _update_space()

    # ── exFAT-only: image size (Auto = smallest safe) ──
    # Auto asks the allocator for its optimal (tight) size and explicitly
    # ignores any stale global Advanced override. Custom forwards a GB
    # value as the per-item override (same mechanism as the legacy
    # per-item dialog). Captured per job at queue time.
    img_size_mode = tk.StringVar(value='auto')
    img_size_gb = tk.StringVar(value='')
    size_frame = tk.Frame(fmt_body, bg=COLORS['bg_2'])
    tk.Label(size_frame, text='exFAT image size:', font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3']).pack(side='left',
                                                         padx=(0, 10))
    for val, lab in (('auto', 'Auto (smallest)'), ('custom', 'Custom:')):
        tk.Radiobutton(size_frame, text=lab, value=val,
                       variable=img_size_mode,
                       font=FONTS['mono_sm'], bg=COLORS['bg_2'],
                       fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'],
                       activebackground=COLORS['bg_2'],
                       activeforeground=COLORS['teal'],
                       highlightthickness=0, bd=0).pack(side='left',
                                                        padx=(0, 8))
    size_ef = tk.Frame(size_frame, bg=COLORS['field_bg'],
                       highlightbackground=COLORS['border_3'],
                       highlightthickness=1)
    size_ef.pack(side='left')
    tk.Entry(size_ef, textvariable=img_size_gb, width=6,
             font=FONTS['mono_sm'], bg=COLORS['field_bg'],
             fg=COLORS['field_fg'], insertbackground=COLORS['field_fg'],
             relief='flat', bd=4, justify='center').pack()
    tk.Label(size_frame, text='GB', font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(side='left',
                                                         padx=(4, 0))

    def _refresh_size_row():
        if out_type.get() == 'exfat':
            size_frame.pack(anchor='w', pady=(12, 0))
        else:
            size_frame.pack_forget()

    # ══════════════════════════════════════════════════════════════════
    # ROW 2 — configure output  +  output name / build stats
    # ══════════════════════════════════════════════════════════════════
    row2 = tk.Frame(inner, bg=COLORS['bg_1'])
    row2.pack(fill='x', padx=24, pady=(0, 12))
    row2.columnconfigure(0, weight=5, uniform='r2')
    row2.columnconfigure(1, weight=6, uniform='r2')

    # ── Configure-output card (folders + build buttons + progress) ──
    cfg_card, cfg_body = _dash_card(row2, '2.  Configure output')
    cfg_card.grid(row=0, column=0, sticky='nsew', padx=(0, 6))

    field_block(cfg_body, 'Output folder', var=outdir_var, on_browse=_browse_out,
                hint='where the image will be written')
    field_block(cfg_body, 'Temp folder (optional)', var=tempdir_var,
                on_browse=_browse_temp,
                hint='redirects the build spool off C: \u2014 applied on build')

    # ── CPU cores selector ──
    # Binds the SAME var as Advanced \u2192 Build Parameters \u2192
    # "Threads (safe)" (robocopy /MT \u2014 parallel file copies), so the
    # two controls stay in sync. Selecting a value clamps + persists
    # via the existing _adv_clamp_threads / _save_adv_params helpers.
    cpu_row = tk.Frame(cfg_body, bg=COLORS['bg_2'])
    cpu_row.pack(fill='x', pady=(12, 0))
    cpu_lcol = tk.Frame(cpu_row, bg=COLORS['bg_2'])
    cpu_lcol.pack(side='left', fill='x', expand=True)
    tk.Label(cpu_lcol, text='CPU cores',
             font=(FONTS['mono_sm'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
             ).pack(fill='x')
    n_cores = max(1, os.cpu_count() or 1)
    tk.Label(cpu_lcol,
             text='parallel file copies during the build '
                  '\u2014 %d available on this machine' % n_cores,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w'
             ).pack(fill='x')

    _core_steps = [1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128]
    core_vals = [str(v) for v in _core_steps if v <= min(n_cores, 128)]
    if str(min(n_cores, 128)) not in core_vals:
        core_vals.append(str(min(n_cores, 128)))
    cur = (app._adv_threads_var.get() or '1').strip()
    if cur not in core_vals:
        try:
            core_vals.append(str(max(1, min(128, int(cur)))))
        except ValueError:
            pass
    core_vals = sorted(set(core_vals), key=int)

    cpu_box = ttk.Combobox(cpu_row, textvariable=app._adv_threads_var,
                           values=core_vals, width=6,
                           state='readonly', font=FONTS['body'])
    cpu_box.pack(side='right', padx=(8, 0))

    def _on_cores_pick(_e=None):
        try:
            app._adv_clamp_threads()
            app._save_adv_params()
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            pass
    cpu_box.bind('<<ComboboxSelected>>', _on_cores_pick)

    btn_row = tk.Frame(cfg_body, bg=COLORS['bg_2'])
    btn_row.pack(fill='x', pady=(16, 0))
    btn_build = make_themed_button(btn_row, text='Build all',
                                   command=lambda: _uq_build_all(),
                                   kind='success', icon='\u25b6',
                                   font_size=13, padx=34, pady=14)
    btn_build.pack(side='right')
    btn_add = make_themed_button(btn_row, text='Add to queue',
                                 command=lambda: _uq_add(),
                                 kind='accent', icon='\u002b',
                                 font_size=9, padx=14, pady=8)
    btn_add.pack(side='right', padx=(0, 8))

    uq_status_lbl = tk.Label(cfg_body, textvariable=uq_status,
                             font=FONTS['mono_sm'], bg=COLORS['bg_2'],
                             fg=COLORS['fg_3'], anchor='w')
    uq_status_lbl.pack(fill='x', pady=(12, 0))

    # live progress (shown only during a run)
    prog_frame = tk.Frame(cfg_body, bg=COLORS['bg_2'])
    prog_top = tk.Frame(prog_frame, bg=COLORS['bg_2'])
    prog_top.pack(fill='x')
    tk.Label(prog_top, textvariable=uq_step_var, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_2'], anchor='w').pack(side='left')
    tk.Label(prog_top, textvariable=uq_pct_var, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='e').pack(side='right')
    track = tk.Frame(prog_frame, bg=COLORS['bg_4'], height=8)
    track.pack(fill='x', pady=(4, 2))
    track.pack_propagate(False)
    fill = tk.Frame(track, bg=COLORS['teal'], height=8)
    fill.place(x=0, y=0, relheight=1, relwidth=0.0)
    tk.Label(prog_frame, textvariable=uq_eta_var, font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w').pack(fill='x')

    # ── Output-name card (preset + name + build stats strip) ──
    name_card, name_body = _dash_card(row2, 'Output name',
                                      head_pady=(12, 0), body_pady=(6, 12))
    name_card.grid(row=0, column=1, sticky='nsew', padx=(6, 0))

    tk.Label(name_body, text='OUTPUT NAME PRESET', font=FONTS['eyebrow'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w').pack(
                 anchor='w', pady=(0, 4))
    preset_row = tk.Frame(name_body, bg=COLORS['bg_2'])
    preset_row.pack(fill='x', pady=(0, 2))
    for val, lab in (('ppsa', 'PPSA only'),
                     ('ppsa_title', 'PPSA + Title'),
                     ('ppsa_title_ver', 'PPSA + Title + Version')):
        tk.Radiobutton(preset_row, text=lab, value=val, variable=name_preset,
                       command=lambda: _gen_name(),
                       font=FONTS['mono_sm'], bg=COLORS['bg_2'],
                       fg=COLORS['fg_2'], selectcolor=COLORS['bg_4'],
                       activebackground=COLORS['bg_2'],
                       activeforeground=COLORS['teal'],
                       highlightthickness=0, bd=0).pack(side='left', padx=(0, 10))

    field_block(name_body, 'Output name', var=name_var,
                hint='auto-filled from the preset above; you can still edit it')

    strip = tk.Frame(name_body, bg=COLORS['bg_3'], highlightthickness=1,
                     highlightbackground=COLORS['border_3'])
    strip.pack(fill='x', pady=(10, 0))
    for c in range(4):
        strip.columnconfigure(c, weight=1, uniform='strip')
    jobs_cell, jobs_val = _strip_cell(strip, 'QUEUE JOBS', stat_jobs_var)
    jobs_cell.grid(row=0, column=0, sticky='ew')
    jobs_val.config(fg=COLORS['fg_4'])      # muted while the queue is empty
    _strip_cell(strip, 'EXPECTED OUTPUT', stat_estsize_var)[0].grid(
        row=0, column=1, sticky='ew')
    needed_cell, needed_val = _strip_cell(strip, 'SPACE NEEDED', stat_needed_var)
    needed_cell.grid(row=0, column=2, sticky='ew')
    space_cell, space_val = _strip_cell(strip, 'OUTPUT SPACE', stat_space_var)
    space_cell.grid(row=0, column=3, sticky='ew')

    # Compression mode — only meaningful for PFS (.ffpfsc container); shown
    # as a full-width second strip row so the 4 stat columns never squeeze.
    comp_cell = _strip_cell(strip, 'COMPRESSION MODE', stat_comp_var)[0]
    comp_cell.grid(row=1, column=0, columnspan=4, sticky='ew')
    comp_cell.grid_remove()

    def _refresh_strip():
        if out_type.get() == 'pfs':
            if pfs_src_mode.get() == 'image':
                stat_comp_var.set('.ffpfsc (compressed)  \u00b7  '
                                  'from existing image (no rebuild)')
            else:
                stat_comp_var.set('.ffpfsc (compressed)  \u00b7  '
                                  'intermediate via %s'
                                  % _TYPE_LABEL[pfs_via.get()])
            comp_cell.grid()
        else:
            comp_cell.grid_remove()

    # ══════════════════════════════════════════════════════════════════
    # Detection / naming / space  (logic preserved)
    # ══════════════════════════════════════════════════════════════════
    def _gen_name(*_):
        folder = src_var.get().strip()
        if not folder:
            return
        ext = _TYPE_EXT[out_type.get()]
        tid = (detected.get('title_id') or '').strip()
        title = (detected.get('title') or '').strip()
        ver = (detected.get('version') or '').strip()
        preset = name_preset.get()
        if preset == 'ppsa':
            base = tid
        elif preset == 'ppsa_title':
            base = (tid + ' ' + title).strip() if title else tid
        else:
            nm = (tid + ' ' + title).strip() if title else tid
            base = (nm + ' (' + ver + ')') if ver else nm
        if not base:
            _bn = os.path.basename(folder.rstrip('/\\'))
            if out_type.get() == 'pfs' and pfs_src_mode.get() == 'image':
                _bn = os.path.splitext(_bn)[0]
            base = _bn
        try:
            from exfat_builder import sanitize_filename
            base = sanitize_filename(base) or base
        except Exception:
            pass
        name_var.set(base + ext)

    def _fmt_b(b):
        if b >= 1024**3:
            return '%.1f GB' % (b / 1024**3)
        if b >= 1024**2:
            return '%.0f MB' % (b / 1024**2)
        return '%.0f KB' % (b / 1024)

    def _fmt_dur(s):
        s = int(s)
        if s >= 3600:
            return '%dh %02dm %02ds' % (s // 3600, (s % 3600) // 60, s % 60)
        if s >= 60:
            return '%dm %02ds' % (s // 60, s % 60)
        return '%ds' % s

    def _refresh_ready():
        src = src_var.get().strip()
        if out_type.get() == 'pfs' and pfs_src_mode.get() == 'image':
            ok = bool(src and os.path.isfile(src)
                      and os.path.splitext(src)[1].lower() in ('.exfat', '.ffpkg'))
        else:
            ok = bool(src and os.path.isdir(src)
                      and (detected.get('title_id') or detected.get('title')))
        if ok:
            ready_badge.pack(side='left')
        else:
            ready_badge.pack_forget()

    def _update_space(*_):
        folder = src_var.get().strip()
        outdir = outdir_var.get().strip()
        t = out_type.get()
        _img_mode = (t == 'pfs' and pfs_src_mode.get() == 'image')
        _src_ok = (os.path.isfile(folder) if _img_mode else os.path.isdir(folder))
        if not folder or not _src_ok:
            for v in (hero_size_var, hero_free_var, stat_estsize_var,
                      stat_needed_var, stat_space_var):
                v.set('\u2014')
            return

        def _w():
            try:
                size = (os.path.getsize(folder) if _img_mode
                        else app._get_folder_size(folder))
            except Exception:
                size = 0
            # image mode packs in place (peak ≈ image size + output);
            # folder mode also builds an intermediate first.
            mult = (1.05 if _img_mode else 2.05) if t == 'pfs' else 1.12
            needed = int(size * mult)
            free = None
            try:
                import shutil as _sh
                if outdir and os.path.isdir(outdir):
                    free = _sh.disk_usage(outdir).free
            except Exception:
                free = None
            short = (free is not None and free < needed)

            def _set():
                hero_size_var.set(_fmt_b(size))
                if t == 'pfs':
                    # same convention as the PFS tab: ~55–95 % after compression
                    stat_estsize_var.set('~%s\u2013%s' % (
                        _fmt_b(size * 0.55), _fmt_b(size * 0.95)))
                else:
                    stat_estsize_var.set('~%s' % _fmt_b(size))
                # peak disk use during the build — the same `needed` figure the
                # low-space warning already checks against (incl. PFS intermediate)
                stat_needed_var.set('~%s' % _fmt_b(needed))
                needed_val.config(fg=COLORS['warn'] if short else COLORS['teal'])
                if free is not None:
                    tag = '  \u26a0' if short else ''
                    hero_free_var.set(_fmt_b(free) + tag)
                    stat_space_var.set(_fmt_b(free) + ' free' + tag)
                    space_val.config(fg=COLORS['warn'] if short else COLORS['teal'])
                    free_val.config(fg=COLORS['warn'] if short else COLORS['fg_1'])
                else:
                    hero_free_var.set('\u2014')
                    stat_space_var.set('\u2014')
                    space_val.config(fg=COLORS['teal'])
            parent.after(0, _set)
        threading.Thread(target=_w, daemon=True).start()

    def _on_source_changed(*_):
        src = src_var.get().strip()
        # PFS "existing image" mode: source is a file, not a dump folder.
        if out_type.get() == 'pfs' and pfs_src_mode.get() == 'image':
            if not src or not os.path.isfile(src):
                return
            if not outdir_var.get().strip():
                outdir_var.set(os.path.dirname(src) or '.')
            import re as _re
            stem = os.path.splitext(os.path.basename(src))[0]
            m = _re.search(r'((?:PPSA|CUSA|PPLH)\d{5})', stem, _re.IGNORECASE)
            tid = m.group(1).upper() if m else ''
            mv = _re.search(r'(\d{2}\.\d{3}\.\d{3})', stem)
            ver = mv.group(1) if mv else ''
            ttl = stem
            if tid:
                ttl = _re.sub('(?i)' + _re.escape(tid), '', ttl)
            if ver:
                # drop a parenthesised version as a unit, then any bare one
                ttl = _re.sub(r'\(\s*' + _re.escape(ver) + r'\s*\)', '', ttl)
                ttl = ttl.replace(ver, '')
            ttl = _re.sub(r'\(\s*\)', '', ttl)   # tidy any empty () left behind
            ttl = _re.sub(r'\s{2,}', ' ', ttl).strip(' -_()')
            detected['title'] = ttl
            detected['title_id'] = tid
            detected['version'] = ver
            detected['folder'] = ''
            det_title_var.set((ttl or tid or stem).upper())
            det_id_var.set(tid or '\u2014')
            hero_ver_var.set(ver or '\u2014')
            cover_ref['img'] = None
            try:
                cover_lbl.config(image='', text='\U0001f5dc',
                                 font=('Segoe UI', 56), width=4, height=4)
            except Exception:
                pass
            _gen_name()
            _refresh_ready()
            _update_space()
            return
        folder = src
        if not folder or not os.path.isdir(folder):
            return
        if not outdir_var.get().strip():
            _par = os.path.dirname(folder.rstrip('/\\'))
            outdir_var.set(_par or folder)
        det_title_var.set('Reading game info\u2026')
        det_id_var.set('')

        def _w():
            title = title_id = version = ''
            try:
                from exfat_builder import get_game_info
                title, title_id, version = get_game_info(folder)
            except Exception:
                pass
            if not title_id:
                import re as _re
                m = _re.search(r'((?:PPSA|CUSA|PPLH)\d{5})',
                               os.path.basename(folder.rstrip('/\\')), _re.IGNORECASE)
                if m:
                    title_id = m.group(1).upper()
            detected['title'] = title
            detected['title_id'] = title_id
            detected['version'] = version
            detected['folder'] = folder
            got_cover = {'ok': False}
            try:
                cp = app._load_cover_art(folder)
                if cp:
                    from exfat_builder import _load_cover_image
                    pil = _load_cover_image(cp, target=200)
                    if pil is not None:
                        def _setimg(p=pil):
                            try:
                                from PIL import ImageTk as _IT
                                cover_ref['img'] = _IT.PhotoImage(p)
                                cover_lbl.config(image=cover_ref['img'], text='',
                                                 width=200, height=200)
                            except Exception:
                                pass
                        got_cover['ok'] = True
                        parent.after(0, _setimg)
            except Exception:
                pass

            def _done():
                if not got_cover['ok']:
                    cover_ref['img'] = None
                    cover_lbl.config(image='', text='\U0001f3ae',
                                     font=('Segoe UI', 56), width=4, height=4)
                disp = (title or title_id
                        or os.path.basename(folder.rstrip('/\\')))
                det_title_var.set(disp.upper())
                det_id_var.set(title_id or '\u2014')
                hero_ver_var.set(version or '\u2014')
                _gen_name()
                _refresh_ready()
            parent.after(0, _done)
            _update_space()
        threading.Thread(target=_w, daemon=True).start()

    src_var.trace_add('write', _on_source_changed)
    outdir_var.trace_add('write', lambda *_: _update_space())

    # ══════════════════════════════════════════════════════════════════
    # Live progress  (logic preserved)
    # ══════════════════════════════════════════════════════════════════
    def _uq_set_progress(pct, step, eta):
        try:
            frac = max(0.0, min(1.0, float(pct) / 100.0))
        except Exception:
            frac = 0.0
        try:
            fill.place_configure(relwidth=frac)
            uq_pct_var.set('%d%%' % int(float(pct)))
            if step is not None:
                uq_step_var.set(str(step))
            if eta is not None:
                uq_eta_var.set(str(eta))
        except Exception:
            pass

    def _uq_progress_show(show):
        if show:
            prog_frame.pack(fill='x', pady=(10, 0))
        else:
            prog_frame.pack_forget()
            _uq_set_progress(0, '', '')

    app._unified_progress = _uq_set_progress

    # ══════════════════════════════════════════════════════════════════
    # Build queue  (card rows; logic preserved)
    # ══════════════════════════════════════════════════════════════════
    qcard = tk.Frame(inner, bg=COLORS['bg_2'], highlightthickness=1,
                     highlightbackground=COLORS['border_2'])
    qhead = tk.Frame(qcard, bg=COLORS['bg_2'])
    qhead.pack(fill='x', padx=20, pady=(16, 4))
    tk.Label(qhead, text='Build queue', font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['accent']).pack(side='left')
    tk.Label(qhead, textvariable=uq_count, font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4']).pack(side='left', padx=(10, 0))
    make_themed_button(qhead, text='Clear', command=lambda: _uq_clear(),
                       kind='accent', icon='\u2717', font_size=9,
                       padx=12, pady=6).pack(side='right')
    make_themed_button(qhead, text='Remove selected',
                       command=lambda: _uq_remove_selected(),
                       kind='accent', icon='\u2715', font_size=9,
                       padx=12, pady=6).pack(side='right', padx=(0, 8))

    uq_list = tk.Frame(qcard, bg=COLORS['bg_2'])
    uq_list.pack(fill='x', padx=14, pady=(0, 16))

    def _uq_render():
        for w in uq_list.winfo_children():
            w.destroy()
        if queue:
            stat_jobs_var.set(str(len(queue)))
            jobs_val.config(fg=COLORS['teal'])
        else:
            stat_jobs_var.set('No queued jobs')
            jobs_val.config(fg=COLORS['fg_4'])
        if not queue:
            qcard.pack_forget()
            uq_count.set('')
            uq_state['sel'].clear()
            return
        qcard.pack(fill='x', padx=24, pady=(0, 12))
        done = sum(1 for j in queue if j['status'] == 'done')
        failed = sum(1 for j in queue if j['status'] == 'failed')
        lab = '%d job%s  \u00b7  %d done' % (
            len(queue), '' if len(queue) == 1 else 's', done)
        if failed:
            lab += '  \u00b7  %d failed' % failed
        uq_count.set(lab)
        present = {id(j) for j in queue}
        for k in [k for k in uq_state['sel'] if k not in present]:
            del uq_state['sel'][k]
        running = uq_state['running']
        n = len(queue)
        for idx, job in enumerate(queue):
            editable = (job['status'] == 'queued' and not running)
            row = tk.Frame(uq_list, bg=COLORS['bg_3'], highlightthickness=1,
                           highlightbackground=COLORS['border_3'])
            row.pack(fill='x', padx=6, pady=4)
            inner_r = tk.Frame(row, bg=COLORS['bg_3'])
            inner_r.pack(fill='x', padx=10, pady=8)
            if editable:
                var = uq_state['sel'].get(id(job))
                if var is None:
                    var = tk.BooleanVar(value=False)
                    uq_state['sel'][id(job)] = var
                tk.Checkbutton(inner_r, variable=var, bg=COLORS['bg_3'],
                               activebackground=COLORS['bg_3'],
                               selectcolor=COLORS['bg_4'], highlightthickness=0,
                               bd=0, cursor='hand2').pack(side='left', padx=(0, 6))
            else:
                tk.Frame(inner_r, bg=COLORS['bg_3'], width=18,
                         height=1).pack(side='left')
            # type badge
            tk.Label(inner_r, text=_TYPE_LABEL[job['type']],
                     font=(FONTS['mono_sm'][0], 8, 'bold'), bg=COLORS['bg_4'],
                     fg=COLORS['teal'], padx=7, pady=2).pack(side='left', padx=(0, 10))
            label = job['name']
            if job['type'] == 'pfs':
                if job.get('from_image'):
                    label += '   from image'
                else:
                    label += '   via %s' % _TYPE_LABEL[job.get('via') or 'exfat']
            tk.Label(inner_r, text=label, font=FONTS['mono_sm'], bg=COLORS['bg_3'],
                     fg=COLORS['fg_1'], anchor='w').pack(
                         side='left', fill='x', expand=True)
            # action icons (only when editable)
            if editable:
                trash = tk.Label(inner_r, text='\U0001f5d1', font=FONTS['meta'],
                                 bg=COLORS['bg_3'], fg=COLORS['fg_5'], cursor='hand2')
                trash.pack(side='right', padx=(8, 4))
                trash.bind('<Button-1>', lambda e, j=job: _uq_remove_job(j))
                if idx < n - 1:
                    dn = tk.Label(inner_r, text='\u25bc', font=FONTS['meta'],
                                  bg=COLORS['bg_3'], fg=COLORS['fg_4'], cursor='hand2')
                    dn.pack(side='right', padx=(0, 4))
                    dn.bind('<Button-1>', lambda e, i=idx: _uq_move(i, +1))
                if idx > 0:
                    up = tk.Label(inner_r, text='\u25b2', font=FONTS['meta'],
                                  bg=COLORS['bg_3'], fg=COLORS['fg_4'], cursor='hand2')
                    up.pack(side='right', padx=(0, 4))
                    up.bind('<Button-1>', lambda e, i=idx: _uq_move(i, -1))
            # duration
            dur = job.get('duration')
            if dur:
                tk.Label(inner_r, text='%ds' % int(dur), font=FONTS['meta'],
                         bg=COLORS['bg_3'], fg=COLORS['fg_4']).pack(
                             side='right', padx=(8, 10))
            # status badge pill
            btxt, bcol = _STATUS_BADGE.get(job['status'], ('?', 'fg_4'))
            tk.Label(inner_r, text=btxt, font=(FONTS['mono_sm'][0], 8, 'bold'),
                     bg=COLORS['bg_4'], fg=COLORS[bcol], padx=8, pady=2).pack(
                         side='right', padx=(8, 0))

    def _uq_add():
        src = src_var.get().strip()
        outdir = outdir_var.get().strip()
        name = name_var.get().strip()
        t = out_type.get()
        _img_mode = (t == 'pfs' and pfs_src_mode.get() == 'image')
        if _img_mode:
            if (not src or not os.path.isfile(src)
                    or os.path.splitext(src)[1].lower() not in ('.exfat', '.ffpkg')):
                messagebox.showerror('Source missing',
                    'Pick an existing .exfat or .ffpkg image.')
                return
        elif not src or not os.path.isdir(src):
            messagebox.showerror('Source missing', 'Pick a valid game dump folder.')
            return
        if not outdir or not os.path.isdir(outdir):
            messagebox.showerror('Output folder missing', 'Pick an output folder.')
            return
        if not _img_mode:
            try:
                if os.path.commonpath([os.path.abspath(src),
                                       os.path.abspath(outdir)]) == os.path.abspath(src):
                    messagebox.showerror('Output folder inside the dump',
                        'The output folder is inside the game dump folder.\n\n'
                        'Pick an output folder OUTSIDE the dump \u2014 writing the image '
                        'into the dump makes the packer try to include its own output '
                        'file, which fails part-way through.')
                    return
            except Exception:
                pass
        ext = _TYPE_EXT[t]
        if not name:
            name = os.path.basename(src.rstrip('/\\')) + ext
        stem = name
        for e in _TYPE_EXT.values():
            if stem.lower().endswith(e):
                stem = stem[:-len(e)]
                break
        name = stem + ext
        exfat_size = 'auto'
        if t == 'exfat' and img_size_mode.get() == 'custom':
            try:
                v = float(img_size_gb.get().strip())
                if v > 0:
                    exfat_size = v
            except Exception:
                exfat_size = 'auto'
        queue.append({
            'src': src, 'outdir': outdir, 'name': name, 'type': t,
            'via': pfs_via.get() if (t == 'pfs' and not _img_mode) else None,
            'from_image': _img_mode,
            'src_ext': os.path.splitext(src)[1].lower() if _img_mode else None,
            'exfat_size': exfat_size if t == 'exfat' else None,
            'status': 'queued',
        })
        _uq_render()

    def _uq_remove_job(job):
        if uq_state['running']:
            return
        try:
            queue.remove(job)
        except ValueError:
            pass
        uq_state['sel'].pop(id(job), None)
        _uq_render()

    def _uq_move(i, delta):
        if uq_state['running']:
            return
        j = i + delta
        if 0 <= j < len(queue):
            queue[i], queue[j] = queue[j], queue[i]
            _uq_render()

    def _uq_remove_selected():
        if uq_state['running']:
            return
        victims = [j for j in queue
                   if id(j) in uq_state['sel'] and uq_state['sel'][id(j)].get()]
        for j in victims:
            try:
                queue.remove(j)
            except ValueError:
                pass
            uq_state['sel'].pop(id(j), None)
        _uq_render()

    def _uq_clear():
        if uq_state['running']:
            return
        queue.clear()
        uq_state['sel'].clear()
        summary_card.pack_forget()
        _uq_render()

    # ══════════════════════════════════════════════════════════════════
    # Build-completion summary card  (additional visual; hidden until done)
    # ══════════════════════════════════════════════════════════════════
    summary_card = tk.Frame(inner, bg=COLORS['bg_2'], highlightthickness=1,
                            highlightbackground=COLORS['success'])
    swrap = tk.Frame(summary_card, bg=COLORS['bg_2'])
    swrap.pack(fill='x', padx=20, pady=16)
    sum_icon = tk.Label(swrap, text='\u2713', font=(FONTS['h2'][0], 22, 'bold'),
                        bg=COLORS['bg_2'], fg=COLORS['success'])
    sum_icon.pack(side='left', padx=(0, 16))
    scol = tk.Frame(swrap, bg=COLORS['bg_2'])
    scol.pack(side='left', fill='x', expand=True)
    tk.Label(scol, textvariable=sum_title_var, font=(FONTS['h3'][0], 12, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(anchor='w')
    tk.Label(scol, textvariable=sum_jobs_var, font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w').pack(anchor='w')
    sgrid = tk.Frame(swrap, bg=COLORS['bg_2'])
    sgrid.pack(side='right')

    def _sum_render(fields):
        """Rebuild the summary stat columns from (label, value) pairs —
        only fields that apply to this run are shown."""
        for w in sgrid.winfo_children():
            w.destroy()
        for col, (label, value) in enumerate(fields):
            f = tk.Frame(sgrid, bg=COLORS['bg_2'])
            f.grid(row=0, column=col, sticky='w', padx=(0, 26))
            tk.Label(f, text=label, font=FONTS['eyebrow'], bg=COLORS['bg_2'],
                     fg=COLORS['fg_5'], anchor='w').pack(anchor='w')
            tk.Label(f, text=value, font=(FONTS['mono_sm'][0], 10, 'bold'),
                     bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w').pack(
                         anchor='w')

    # ══════════════════════════════════════════════════════════════════
    # Dispatch + runner  (logic preserved verbatim, plus duration / stats)
    # ══════════════════════════════════════════════════════════════════
    def _set_buttons(enabled):
        st = 'normal' if enabled else 'disabled'
        for b in (btn_add, btn_build):
            try:
                b.config(state=st)
            except Exception:
                pass

    def _dispatch_exfat(job, cb):
        try:
            from exfat_builder import QueueItem
            item = QueueItem(job['src'], job['outdir'], job['name'],
                             game_title=os.path.basename(job['src'].rstrip('/\\')),
                             size_override_gb=job.get('exfat_size') or 'auto')
            app._queue.append(item)
            idx = len(app._queue) - 1
            uq_state.setdefault('added', []).append(item)
            try:
                app._render_queue()
            except Exception:
                pass
            app._unified_active = True
            app._unified_job_done = cb
            app._building = True
            try:
                app.build_btn.config(state='disabled', text='Building\u2026')
            except Exception:
                pass
            app._process_next([idx], 0)
        except Exception as e:
            try:
                app._log('[UNIBUILD] exFAT dispatch error: %s\n' % e)
            except Exception:
                pass
            cb(False)

    def _dispatch_ffpkg(job, cb):
        try:
            if not getattr(app, '_ffpkg_ufs2_exe', None) \
                    or not os.path.isfile(app._ffpkg_ufs2_exe):
                from exfat_builder import extract_ufs2tool
                temp_dir = app._settings.get('temp_dir') or None
                app._ffpkg_ufs2_exe = extract_ufs2tool(temp_dir)
            app._ffpkg_queue.append({
                'folder': job['src'], 'odir': job['outdir'],
                'name': job['name'], 'status': 'waiting',
            })
            idx = len(app._ffpkg_queue) - 1
            uq_state.setdefault('added_ffpkg', []).append(app._ffpkg_queue[idx])
            try:
                app._ffpkg_render_queue()
            except Exception:
                pass
            app._unified_active = True
            app._unified_job_done = cb
            app._ffpkg_building = True
            try:
                app._ffpkg_build_btn.config(state='disabled', text='Building...')
            except Exception:
                pass
            app._ffpkg_process_next([idx], 0)
        except Exception as e:
            try:
                app._log('[UNIBUILD] ffpkg dispatch error: %s\n' % e)
            except Exception:
                pass
            cb(False)

    def _dispatch_pfs(job, cb):
        via = (job.get('via') or 'exfat')
        final = job['name']
        stem = (final[:-len('.ffpfsc')]
                if final.lower().endswith('.ffpfsc') else final)
        inter_ext = '.exfat' if via == 'exfat' else '.ffpkg'
        inter_name = stem + inter_ext
        inter_path = os.path.normpath(os.path.join(job['outdir'], inter_name))
        final_path = os.path.normpath(os.path.join(job['outdir'], stem + '.ffpfsc'))
        inter_job = {'src': job['src'], 'outdir': job['outdir'], 'name': inter_name}

        def _after_build(ok):
            if not ok:
                cb(False)
                return
            if not os.path.isfile(inter_path):
                try:
                    app._log('[UNIBUILD] PFS: intermediate not found: %s\n' % inter_path)
                except Exception:
                    pass
                cb(False)
                return
            try:
                job['_inter_size'] = os.path.getsize(inter_path)
            except Exception:
                job['_inter_size'] = 0
            uq_status.set('Converting %s \u2192 .ffpfsc\u2026' % inter_name)
            _uq_set_progress(0, 'Converting \u2192 .ffpfsc', 'compressing\u2026')

            def _after_convert():
                ok2 = (os.path.isfile(final_path)
                       and not os.path.isfile(inter_path))
                if ok2:
                    _uq_set_progress(100, 'Converted to .ffpfsc', 'done')
                    try:
                        fsz = os.path.getsize(final_path)
                        isz = job.get('_inter_size', 0)
                        job['out_path'] = final_path
                        job['space_saved'] = max(0, isz - fsz)
                        job['compression'] = ((1 - fsz / isz) * 100) if isz else 0
                    except Exception:
                        pass
                cb(ok2)

            try:
                app._pipeline_convert_then_next(inter_path, _after_convert)
            except Exception as e:
                try:
                    app._log('[UNIBUILD] PFS convert error: %s\n' % e)
                except Exception:
                    pass
                cb(False)

        if via == 'ffpkg':
            _dispatch_ffpkg(inter_job, _after_build)
        else:
            _dispatch_exfat(inter_job, _after_build)

    def _dispatch_pfs_from_image(job, cb):
        # Pack an existing .exfat / .ffpkg straight into a .ffpfsc — no
        # intermediate build, no extraction. The user's original file is
        # never modified: stage a hard link (or a copy across volumes) in a
        # temp subfolder, let the pack step consume/delete the stage, then
        # move the result to the chosen output name.
        import shutil as _sh
        src_image = job['src']
        src_ext = (job.get('src_ext')
                   or os.path.splitext(src_image)[1].lower() or '.exfat')
        final = job['name']
        stem = (final[:-len('.ffpfsc')]
                if final.lower().endswith('.ffpfsc') else final)
        outdir = job['outdir']
        final_path = os.path.normpath(os.path.join(outdir, stem + '.ffpfsc'))
        # ASCII-safe staged stem so mkpfs can encode the nested entry name
        # (0.0.6 fails fast on non-ASCII); the final .ffpfsc keeps the
        # user's chosen name via the move below.
        ascii_stem = (stem.encode('ascii', 'ignore').decode('ascii')
                      .strip().strip('.') or 'image')
        stage_dir = os.path.join(outdir, '.pfs_stage')
        try:
            if os.path.isdir(stage_dir):
                _sh.rmtree(stage_dir, ignore_errors=True)
            os.makedirs(stage_dir, exist_ok=True)
        except Exception as e:
            try:
                app._log('[UNIBUILD] PFS(image): cannot create stage dir: %s\n' % e)
            except Exception:
                pass
            cb(False)
            return
        staged = os.path.join(stage_dir, ascii_stem + src_ext)
        staged_ffpfsc = os.path.splitext(staged)[0] + '.ffpfsc'
        try:
            try:
                os.link(src_image, staged)            # instant, same volume
            except Exception:
                _sh.copyfile(src_image, staged)       # cross-volume fallback
            job['_inter_size'] = os.path.getsize(src_image)
        except Exception as e:
            try:
                app._log('[UNIBUILD] PFS(image): staging failed: %s\n' % e)
            except Exception:
                pass
            _sh.rmtree(stage_dir, ignore_errors=True)
            cb(False)
            return

        def _after_convert():
            ok2 = os.path.isfile(staged_ffpfsc)
            out_final = final_path
            if ok2:
                try:
                    if os.path.normcase(staged_ffpfsc) != os.path.normcase(final_path):
                        if os.path.exists(final_path):
                            os.remove(final_path)
                        os.replace(staged_ffpfsc, final_path)
                except Exception as e:
                    try:
                        app._log('[UNIBUILD] PFS(image): could not move to %s '
                                 '(%s) — keeping staged name.\n'
                                 % (final_path, e))
                    except Exception:
                        pass
                    out_final = staged_ffpfsc
            ok2 = os.path.isfile(out_final)
            if ok2:
                try:
                    fsz = os.path.getsize(out_final)
                    isz = job.get('_inter_size', 0)
                    job['out_path'] = out_final
                    job['space_saved'] = max(0, isz - fsz)
                    job['compression'] = ((1 - fsz / isz) * 100) if isz else 0
                except Exception:
                    pass
            _sh.rmtree(stage_dir, ignore_errors=True)
            cb(bool(ok2))

        uq_status.set('Packing %s → .ffpfsc…'
                      % os.path.basename(src_image))
        _uq_set_progress(0, 'Packing → .ffpfsc', 'compressing…')
        try:
            app._pipeline_convert_then_next(staged, _after_convert)
        except Exception as e:
            try:
                app._log('[UNIBUILD] PFS(image) pack error: %s\n' % e)
            except Exception:
                pass
            _sh.rmtree(stage_dir, ignore_errors=True)
            cb(False)

    def _uq_dispatch(job, cb):
        t = job['type']
        if t == 'exfat':
            _dispatch_exfat(job, cb)
        elif t == 'ffpkg':
            _dispatch_ffpkg(job, cb)
        elif t == 'pfs':
            if job.get('from_image'):
                _dispatch_pfs_from_image(job, cb)
            else:
                _dispatch_pfs(job, cb)
        else:
            cb(None)

    def _uq_done(i, ok):
        job = queue[i]
        try:
            job['duration'] = max(0, time.time() - job.get('_t0', time.time()))
        except Exception:
            pass
        if job['type'] != 'pfs' and 'out_path' not in job:
            job['out_path'] = os.path.join(job['outdir'], job['name'])
        if ok is None:
            job['status'] = 'skipped'
        else:
            job['status'] = 'done' if ok else 'failed'
        app._unified_job_done = None
        _uq_render()
        parent.after(60, lambda: _uq_run_idx(i + 1))

    def _uq_run_idx(i):
        if i >= len(queue):
            _uq_finish()
            return
        job = queue[i]
        if job['status'] != 'queued':
            _uq_run_idx(i + 1)
            return
        job['status'] = 'running'
        job['_t0'] = time.time()
        uq_status.set('Building %d/%d  \u00b7  %s  (%s)\u2026'
                      % (i + 1, len(queue), job['name'], _TYPE_LABEL[job['type']]))
        _uq_set_progress(0, 'Starting %s\u2026' % job['name'], '')
        _uq_render()
        _uq_dispatch(job, lambda ok, i=i: _uq_done(i, ok))

    def _uq_finish():
        uq_state['running'] = False
        app._unified_active = False
        app._building = False
        app._ffpkg_building = False
        for it in uq_state.get('added', []):
            try:
                app._queue.remove(it)
            except ValueError:
                pass
        uq_state['added'] = []
        for it in uq_state.get('added_ffpkg', []):
            try:
                app._ffpkg_queue.remove(it)
            except ValueError:
                pass
        uq_state['added_ffpkg'] = []
        try:
            app._render_queue()
            app.build_btn.config(state='normal', text='Build All')
        except Exception:
            pass
        try:
            app._ffpkg_render_queue()
            app._ffpkg_build_btn.config(state='normal', text='Build All')
        except Exception:
            pass
        _set_buttons(True)
        uq_status.set('')
        _uq_progress_show(False)
        _uq_render()

        done_jobs = [j for j in queue if j['status'] == 'done']
        done = len(done_jobs)
        failed = sum(1 for j in queue if j['status'] == 'failed')
        total_dur = sum(j.get('duration', 0) for j in queue)

        line = '%d job%s finished  \u00b7  %d failed' % (
            done, '' if done == 1 else 's', failed)
        if len(queue) > 1 and done_jobs:
            counts = {}
            for j in done_jobs:
                counts[j['type']] = counts.get(j['type'], 0) + 1
            line += '  \u00b7  ' + '  \u00b7  '.join(
                '%s \u00d7%d' % (_TYPE_LABEL[t], n) for t, n in counts.items())
        sum_jobs_var.set(line)

        # totals from metrics captured during the run; for exFAT/ffpkg jobs
        # the written file is sized directly (display only)
        total_out = 0
        for j in done_jobs:
            isz = j.get('_inter_size') or 0
            if isz:
                total_out += max(0, isz - (j.get('space_saved') or 0))
            else:
                try:
                    p = j.get('out_path')
                    if p and os.path.isfile(p):
                        total_out += os.path.getsize(p)
                except Exception:
                    pass
        inter_total = sum((j.get('_inter_size') or 0) for j in done_jobs)
        saved_total = sum((j.get('space_saved') or 0) for j in done_jobs
                          if j.get('_inter_size'))

        if done == 1:
            last = done_jobs[0]
            fields = [('OUTPUT FILE',
                       os.path.basename(last.get('out_path') or last['name']))]
        else:
            fields = [('OUTPUT', ('%d files' % done) if done else '\u2014')]
        fields.append(('TOTAL SIZE', _fmt_b(total_out) if total_out else '\u2014'))
        fields.append(('DURATION', _fmt_dur(total_dur) if total_dur else '\u2014'))
        if inter_total:
            fields.append(('COMPRESSION',
                           '%.1f%%' % (saved_total / inter_total * 100.0)))
            fields.append(('SPACE SAVED', _fmt_b(saved_total)))
        _sum_render(fields)

        if failed:
            sum_title_var.set('BUILD FINISHED \u2014 %d ERROR%s'
                              % (failed, '' if failed == 1 else 'S'))
            sum_icon.config(text='\u26a0', fg=COLORS['warn'])
        else:
            sum_title_var.set('BUILD COMPLETE')
            sum_icon.config(text='\u2713', fg=COLORS['success'])
        summary_card.config(highlightbackground=COLORS['success'] if not failed
                            else COLORS['warn'])
        summary_card.pack(fill='x', padx=24, pady=(0, 16))

        _toast(app, 'Build complete' if not failed else 'Build finished',
               '%d job%s finished' % (done, '' if done == 1 else 's'),
               '%d failed' % failed, ok=(failed == 0))

    def _uq_build_all():
        if uq_state['running']:
            return
        if not queue:
            messagebox.showinfo('Build queue', 'Add some jobs first.')
            return
        if getattr(app, '_building', False) or getattr(app, '_ffpkg_building', False):
            messagebox.showwarning('Busy',
                'A build is already running on another tab \u2014 let it finish first.')
            return
        if not any(j['status'] in ('queued', 'failed', 'skipped') for j in queue):
            messagebox.showinfo('Build queue', 'Nothing left to build.')
            return
        for j in queue:
            if j['status'] in ('failed', 'skipped'):
                j['status'] = 'queued'
                j.pop('duration', None)
        try:
            from exfat_builder import save_settings
            tv = tempdir_var.get().strip()
            if tv != (app._settings.get('temp_dir') or ''):
                app._settings['temp_dir'] = tv
                save_settings(app._settings)
        except Exception:
            pass
        summary_card.pack_forget()
        uq_state['running'] = True
        uq_state['added'] = []
        uq_state['added_ffpkg'] = []
        app._unified_active = True
        _set_buttons(False)
        _uq_progress_show(True)
        _uq_render()
        _uq_run_idx(0)

    # initial state
    _restyle_tiles()
    _refresh_via()
    _refresh_size_row()
    _refresh_strip()
    _refresh_ready()
