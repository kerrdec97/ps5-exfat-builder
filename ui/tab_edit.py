"""ui/tab_edit.py — unified Edit workflow tab (Stage C).

One visible Edit tab that opens all supported image formats and routes
each to the right editor, WITHOUT building a second editor for any
format:

  .ffpkg            -> the existing UFS2Tool editor (build_ffpkg_edit_tab),
                       byte-unchanged; opened by path via open_ffpkg_path.
  .exfat            -> the existing mount-based Files editor
                       (build_files_tab); opened by setting _fm_image_var
                       and calling the app's own _fm_mount().
  .ffpfs / .ffpfsc  -> workspace mode (mkpfs unpack -> edit -> rebuild).
                       Never edits a PFS image in place; never overwrites
                       the source. A .ffpfsc whose nested image is a
                       .ffpkg / .exfat is handed straight to the matching
                       editor above. Rebuild is routed to the Build tab
                       (which writes a new file), so no repack logic or
                       overwrite risk lives here.

Format detection is extension-based. An active-format/mode pill is shown
at the top. The three editor panes are stacked; only the active one is
packed. Each pane keeps its own native controls, so the unified opener is
a convenience layered on top of fully-working editors.

OSFMount cleanup is the app's existing close-time sweep
(_sweep_stale_osf_mounts) — nothing new is registered here.
"""

import os
import threading
import tkinter as tk
from tkinter import filedialog, messagebox

from tkinter_theme import COLORS, FONTS
from exfat_builder import _

from ui.tab_ffpkg_edit import build_ffpkg_edit_tab, open_ffpkg_path
from ui.tab_files import build_files_tab


_MODE_PILL = {
    'ffpkg': ('FFPKG \u00b7 in-place edit', 'teal'),
    'exfat': ('exFAT \u00b7 mounted edit', 'accent_hi'),
    'pfs':   ('PFS \u00b7 workspace',       'warn'),
    'none':  ('No image open',              'fg_4'),
}


def _detect_format(path):
    """'ffpkg' | 'exfat' | 'pfs' (.ffpfs) | 'pfs_c' (.ffpfsc) | None."""
    p = (path or '').lower()
    if p.endswith('.ffpkg'):
        return 'ffpkg'
    if p.endswith('.exfat'):
        return 'exfat'
    if p.endswith('.ffpfsc'):
        return 'pfs_c'
    if p.endswith('.ffpfs'):
        return 'pfs'
    return None


def build_edit_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    state = {'mode': 'ffpkg', 'pfs_src': '', 'pfs_compressed': False}

    # ── Header: title + active-format pill + unified Open + segments ──
    head = tk.Frame(parent, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 6))

    icon_tile = tk.Frame(head, bg=COLORS['accent_15'], width=32, height=32)
    icon_tile.pack(side='left')
    icon_tile.pack_propagate(False)
    tk.Label(icon_tile, text='\u270f', bg=COLORS['accent_15'],
             fg=COLORS['accent'], font=('Segoe UI', 14)).pack(expand=True)

    title_col = tk.Frame(head, bg=COLORS['bg_1'])
    title_col.pack(side='left', padx=(10, 0))
    tk.Label(title_col, text=_('Edit'), font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']).pack(anchor='w')
    tk.Label(title_col,
             text=_('Open a .ffpkg, .exfat, or PFS image \u2014 the editor '
                    'is chosen automatically from the file.'),
             font=FONTS['mono_sm'], bg=COLORS['bg_1'],
             fg=COLORS['fg_5']).pack(anchor='w')

    # Active-format pill (right side)
    pill_var = tk.StringVar(value=_MODE_PILL['none'][0])
    pill = tk.Label(head, textvariable=pill_var,
                    font=(FONTS['mono_sm'][0], 9, 'bold'),
                    bg=COLORS['bg_3'], fg=COLORS['fg_4'], padx=10, pady=4,
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    pill.pack(side='right')

    def _open():
        _edit_open(app, state, _show_mode, _set_pill, pfs_api)
    from ui.shared.page_head import make_themed_button
    make_themed_button(head, _('Open image\u2026'), command=_open,
                       kind='accent', icon='\U0001f4c2',
                       font_size=10).pack(side='right', padx=(0, 10))

    # ── Format selector strip (manual override) ──
    seg_row = tk.Frame(parent, bg=COLORS['bg_1'])
    seg_row.pack(fill='x', padx=24, pady=(0, 8))
    seg_btns = {}

    def _seg(key, label):
        b = tk.Label(seg_row, text=label, font=(FONTS['mono_sm'][0], 9, 'bold'),
                     bg=COLORS['bg_3'], fg=COLORS['fg_3'], padx=14, pady=6,
                     cursor='hand2', highlightbackground=COLORS['border_3'],
                     highlightthickness=1)
        b.pack(side='left', padx=(0, 6))
        b.bind('<Button-1>', lambda e, k=key: _show_mode(k))
        seg_btns[key] = b

    _seg('ffpkg', '\U0001f4e6  ffpkg')
    _seg('exfat', '\U0001f5c2  exFAT')
    _seg('pfs',   '\U0001f5dc  PFS')

    # ── Stacked editor panes ──
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    ffpkg_pane = tk.Frame(body, bg=COLORS['bg_1'])
    exfat_pane = tk.Frame(body, bg=COLORS['bg_1'])
    pfs_pane = tk.Frame(body, bg=COLORS['bg_1'])

    # Reuse the existing editors verbatim (no second editor is created).
    build_ffpkg_edit_tab(ffpkg_pane, app)
    build_files_tab(exfat_pane, app)
    pfs_api = _build_pfs_workspace(pfs_pane, app, state)

    panes = {'ffpkg': ffpkg_pane, 'exfat': exfat_pane, 'pfs': pfs_pane}

    def _set_pill(mode_key):
        text, color = _MODE_PILL.get(mode_key, _MODE_PILL['none'])
        pill_var.set(text)
        try:
            pill.config(fg=COLORS[color])
        except Exception:
            pass

    def _show_mode(mode):
        state['mode'] = mode
        for k, fr in panes.items():
            try:
                fr.pack_forget()
            except Exception:
                pass
        panes[mode].pack(fill='both', expand=True)
        for k, b in seg_btns.items():
            sel = (k == mode)
            b.config(bg=COLORS['accent_08'] if sel else COLORS['bg_3'],
                     fg=COLORS['teal'] if sel else COLORS['fg_3'],
                     highlightbackground=COLORS['teal'] if sel
                     else COLORS['border_3'])
        # keep the pill honest: only show an active edit-mode when one of
        # the editors actually holds an image
        if mode == 'pfs':
            _set_pill('pfs')

    # Expose a couple of hooks the PFS workspace uses for handoff.
    pfs_api['show_mode'] = _show_mode
    pfs_api['set_pill'] = _set_pill

    _show_mode('ffpkg')
    _set_pill('none')


def _edit_open(app, state, show_mode, set_pill, pfs_api):
    p = filedialog.askopenfilename(
        title=_('Open image to edit'),
        filetypes=[
            (_('Supported images'), '*.ffpkg *.exfat *.ffpfs *.ffpfsc'),
            ('ffpkg images', '*.ffpkg'),
            ('exFAT images', '*.exfat'),
            ('PFS images', '*.ffpfs *.ffpfsc'),
            (_('All files'), '*.*')])
    if not p:
        return
    p = p.replace('/', '\\')
    fmt = _detect_format(p)
    if fmt == 'ffpkg':
        show_mode('ffpkg')
        open_ffpkg_path(app, p)
        set_pill('ffpkg')
    elif fmt == 'exfat':
        show_mode('exfat')
        try:
            app._fm_image_var.set(p)
            app._fm_mount()
            set_pill('exfat')
        except Exception:
            pass
    elif fmt in ('pfs', 'pfs_c'):
        show_mode('pfs')
        pfs_api['set_source'](p, compressed=(fmt == 'pfs_c'))
        set_pill('pfs')
    else:
        messagebox.showwarning(_('Unsupported file type'),
            _('Supported image types: .ffpkg, .exfat, .ffpfs, .ffpfsc'))


# ─────────────────────────────────────────────────────────────────────────
# PFS workspace mode — unpack -> edit -> rebuild (never in place)
# ─────────────────────────────────────────────────────────────────────────
def _build_pfs_workspace(pane, app, state):
    api = {}
    wrap = tk.Frame(pane, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True, padx=24, pady=(6, 16))

    # ── 4-step workflow strip (Select -> Unpack -> Edit -> Rebuild) ──
    steps_row = tk.Frame(wrap, bg=COLORS['bg_1'])
    steps_row.pack(fill='x', pady=(0, 14))
    _STEP_DEFS = [
        ('1', _('Select PFS Image'), '\U0001f5dc', _('Choose a .ffpfs or .ffpfsc.')),
        ('2', _('Unpack to Workspace'), '\U0001f4e4',
         _('Extract contents to a folder.')),
        ('3', _('Edit Files'), '\U0001f4dd',
         _('Edit files (or the nested image).')),
        ('4', _('Rebuild Image'), '\U0001f4e6',
         _('Rebuild a NEW image from Build.')),
    ]
    step_refs = []
    for i, (num, title, glyph, sub) in enumerate(_STEP_DEFS):
        steps_row.columnconfigure(i, weight=1, uniform='step')
        sc = tk.Frame(steps_row, bg=COLORS['bg_2'],
                      highlightbackground=COLORS['border_2'],
                      highlightthickness=1)
        sc.grid(row=0, column=i, sticky='nsew',
                padx=(0 if i == 0 else 8, 0))
        sp = tk.Frame(sc, bg=COLORS['bg_2'])
        sp.pack(fill='both', expand=True, padx=14, pady=12)
        top = tk.Frame(sp, bg=COLORS['bg_2'])
        top.pack(fill='x')
        eyebrow = tk.Label(top, text=_('STEP ') + num,
                           font=(FONTS['mono_sm'][0], 8, 'bold'),
                           bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w')
        eyebrow.pack(side='left')
        chk = tk.Label(top, text='', font=(FONTS['mono_sm'][0], 10, 'bold'),
                       bg=COLORS['bg_2'], fg=COLORS['success'])
        chk.pack(side='right')
        gl = tk.Label(sp, text=glyph, font=('Segoe UI', 22),
                      bg=COLORS['bg_2'], fg=COLORS['fg_5'], anchor='w')
        gl.pack(anchor='w', pady=(8, 4))
        tl = tk.Label(sp, text=title, font=(FONTS['h3'][0], 11, 'bold'),
                      bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='w',
                      justify='left', wraplength=150)
        tl.pack(anchor='w', fill='x')
        tk.Label(sp, text=sub, font=FONTS['meta'], bg=COLORS['bg_2'],
                 fg=COLORS['fg_4'], anchor='w', justify='left',
                 wraplength=150).pack(anchor='w', pady=(2, 0))
        step_refs.append({'card': sc, 'eyebrow': eyebrow, 'chk': chk,
                          'glyph': gl, 'title': tl})

    def _set_step(idx, st):
        # st: 'done' | 'active' | 'pending'
        r = step_refs[idx]
        if st == 'done':
            r['card'].config(highlightbackground=COLORS['success'],
                             highlightthickness=2)
            r['chk'].config(text='\u2713')
            r['glyph'].config(fg=COLORS['success'])
            r['title'].config(fg=COLORS['fg_0'])
        elif st == 'active':
            r['card'].config(highlightbackground=COLORS['teal'],
                             highlightthickness=2)
            r['chk'].config(text='')
            r['glyph'].config(fg=COLORS['teal'])
            r['title'].config(fg=COLORS['teal'])
        else:
            r['card'].config(highlightbackground=COLORS['border_2'],
                             highlightthickness=1)
            r['chk'].config(text='')
            r['glyph'].config(fg=COLORS['fg_5'])
            r['title'].config(fg=COLORS['fg_1'])

    def _steps(active_idx, done_upto):
        for i in range(4):
            if i < done_upto:
                _set_step(i, 'done')
            elif i == active_idx:
                _set_step(i, 'active')
            else:
                _set_step(i, 'pending')
    _steps(0, 0)

    card = tk.Frame(wrap, bg=COLORS['bg_2'], highlightthickness=1,
                    highlightbackground=COLORS['border_2'])
    card.pack(fill='x')
    pad = tk.Frame(card, bg=COLORS['bg_2'])
    pad.pack(fill='x', padx=20, pady=16)

    tk.Label(pad, text=_('PFS workspace mode'),
             font=(FONTS['h3'][0], 12, 'bold'), bg=COLORS['bg_2'],
             fg=COLORS['accent'], anchor='w').pack(anchor='w')
    tk.Label(pad, text=_('PFS images are not edited in place. Unpack to a '
                         'workspace, edit the contents (or the nested '
                         'image), then rebuild to a NEW file from the Build '
                         'tab \u2014 the source is never modified.'),
             font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w', justify='left', wraplength=720).pack(
                 anchor='w', pady=(2, 12))

    src_var = tk.StringVar(value='')
    ws_var = tk.StringVar(value='')

    def _row(label_text, var, browse_cmd, hint):
        tk.Label(pad, text=label_text, font=FONTS['label'], bg=COLORS['bg_2'],
                 fg=COLORS['fg_3'], anchor='w').pack(anchor='w', pady=(6, 2))
        line = tk.Frame(pad, bg=COLORS['field_bg'],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1)
        line.pack(fill='x')
        tk.Entry(line, textvariable=var, font=FONTS['mono_sm'],
                 bg=COLORS['field_bg'], fg=COLORS['field_fg'],
                 insertbackground=COLORS['field_fg'], relief='flat',
                 bd=8).pack(side='left', fill='x', expand=True)
        if browse_cmd:
            from ui.shared.page_head import make_themed_button
            make_themed_button(line, _('Browse'), command=browse_cmd,
                               kind='ghost', font_size=9).pack(side='right')

    def _browse_ws():
        d = filedialog.askdirectory(title=_('Select workspace folder'))
        if d:
            ws_var.set(d)

    _row(_('Source PFS image'), src_var, None, '')
    _row(_('Workspace folder'), ws_var, _browse_ws, '')

    btn_row = tk.Frame(pad, bg=COLORS['bg_2'])
    btn_row.pack(fill='x', pady=(14, 0))
    from ui.shared.page_head import make_themed_button
    unpack_btn = make_themed_button(btn_row, _('Unpack to workspace'),
                                    command=lambda: _unpack(),
                                    kind='success', icon='\u25b6',
                                    font_size=10, padx=18, pady=9)
    unpack_btn.pack(side='left')

    status_var = tk.StringVar(value='')
    tk.Label(pad, textvariable=status_var, font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='w').pack(
                 fill='x', pady=(12, 0))

    # action area — repopulated after a successful unpack
    actions = tk.Frame(pad, bg=COLORS['bg_2'])
    actions.pack(fill='x', pady=(8, 0))

    def _clear_actions():
        for w in actions.winfo_children():
            w.destroy()

    def _go_build():
        try:
            app._switch_tab('unibuild')
        except Exception:
            pass

    def set_source(path, compressed=False):
        state['pfs_src'] = path
        state['pfs_compressed'] = compressed
        src_var.set(path)
        if path and not ws_var.get().strip():
            ws_var.set(os.path.dirname(path))
        status_var.set('')
        _clear_actions()
        _steps(1, 1)   # step 1 done, step 2 active

    api['set_source'] = set_source

    def _after_unpack(dest):
        _clear_actions()
        _steps(2, 2)   # steps 1-2 done, step 3 (edit) active
        nested = None
        kind = None
        if state['pfs_compressed']:
            try:
                for f in sorted(os.listdir(dest)):
                    fp = os.path.join(dest, f)
                    if not os.path.isfile(fp):
                        continue
                    low = f.lower()
                    if low.endswith('.ffpkg'):
                        nested, kind = fp, 'ffpkg'
                        break
                    if low.endswith('.exfat'):
                        nested, kind = fp, 'exfat'
                        break
                    if low == 'pfs_image.dat':
                        nested, kind = fp, 'raw'
            except Exception:
                pass

        if kind == 'ffpkg':
            tk.Label(actions,
                     text=_('Nested .ffpkg found \u2014 edit it directly:'),
                     font=FONTS['meta'], bg=COLORS['bg_2'],
                     fg=COLORS['fg_3'], anchor='w').pack(anchor='w',
                                                          pady=(0, 4))
            make_themed_button(actions, _('Edit nested .ffpkg here'),
                command=lambda np=nested: _handoff_ffpkg(np),
                kind='accent', icon='\U0001f4e6', font_size=10).pack(
                    side='left')
        elif kind == 'exfat':
            tk.Label(actions,
                     text=_('Nested .exfat found \u2014 mount and edit it:'),
                     font=FONTS['meta'], bg=COLORS['bg_2'],
                     fg=COLORS['fg_3'], anchor='w').pack(anchor='w',
                                                          pady=(0, 4))
            make_themed_button(actions, _('Edit nested .exfat here'),
                command=lambda np=nested: _handoff_exfat(np),
                kind='accent', icon='\U0001f5c2', font_size=10).pack(
                    side='left')
        else:
            msg = (_('Unpacked the nested raw PFS image (pfs_image.dat). '
                     'It cannot be edited directly here.')
                   if kind == 'raw'
                   else _('Unpacked the game files. Edit them on disk, then '
                          'rebuild a new .ffpfsc from the Build tab.'))
            tk.Label(actions, text=msg, font=FONTS['meta'], bg=COLORS['bg_2'],
                     fg=COLORS['fg_3'], anchor='w', justify='left',
                     wraplength=720).pack(anchor='w', pady=(0, 6))
            make_themed_button(actions, _('Rebuild in Build tab'),
                command=_go_build, kind='ghost', font_size=10).pack(
                    side='left')

    def _handoff_ffpkg(path):
        if 'show_mode' in api:
            api['show_mode']('ffpkg')
        open_ffpkg_path(app, path)
        if 'set_pill' in api:
            api['set_pill']('ffpkg')

    def _handoff_exfat(path):
        if 'show_mode' in api:
            api['show_mode']('exfat')
        try:
            app._fm_image_var.set(path)
            app._fm_mount()
            if 'set_pill' in api:
                api['set_pill']('exfat')
        except Exception:
            pass

    def _unpack():
        src = src_var.get().strip()
        ws = ws_var.get().strip()
        if not src or not os.path.isfile(src):
            messagebox.showwarning(_('Missing'),
                _('No PFS image selected.'))
            return
        if not ws or not os.path.isdir(ws):
            messagebox.showwarning(_('Missing'),
                _('Pick a workspace folder.'))
            return
        stem = os.path.splitext(os.path.basename(src))[0]
        dest = os.path.normpath(os.path.join(ws, stem))
        overwrite = os.path.exists(dest)
        if overwrite and not messagebox.askyesno(_('Folder exists'),
                dest + _('\n\nalready exists. Overwrite its contents?')):
            return
        unpack_btn.config(state='disabled')
        status_var.set(_('Unpacking\u2026'))

        def _worker():
            try:
                from ui.mkpfs_runner import run_mkpfs, mkpfs_version
                if not mkpfs_version():
                    pane.after(0, lambda: (
                        status_var.set(_('mkpfs not available')),
                        unpack_btn.config(state='normal')))
                    return
                argv = ['unpack', src, dest]
                if overwrite:
                    argv.append('--overwrite')

                def _log(l):
                    try:
                        app._log('[EDIT/PFS] %s\n' % l)
                    except Exception:
                        pass
                _log('mkpfs ' + ' '.join(argv))

                def _prog(phase, pct, total, detail):
                    pane.after(0, lambda d=detail, p=pct:
                        status_var.set(_('Unpacking \u00b7 %s (%d%%)')
                                       % (d or phase, int(p))))

                rc = run_mkpfs(argv, log_cb=_log, progress_cb=_prog)
                if rc == 0 and os.path.isdir(dest):
                    pane.after(0, lambda: (
                        status_var.set(_('Unpacked to: %s') % dest),
                        unpack_btn.config(state='normal'),
                        _after_unpack(dest)))
                else:
                    pane.after(0, lambda: (
                        status_var.set(_('Unpack failed (rc=%s)') % rc),
                        unpack_btn.config(state='normal')))
            except Exception as e:
                pane.after(0, lambda err=e: (
                    status_var.set(_('Unpack error: %r') % err),
                    unpack_btn.config(state='normal')))
        threading.Thread(target=_worker, daemon=True).start()

    return api
