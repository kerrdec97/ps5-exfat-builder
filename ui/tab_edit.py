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
from tkinter import filedialog

from ui.shared.app_dialog import app_confirm, app_warning

from tkinter_theme import COLORS, FONTS, px
from exfat_builder import _

from ui.shared.widgets import (PageHeader, Card, Badge, Button, IconLabel,
                               Rounded, Segmented, autowrap)
from ui.shared.page_head import field_block
from ui.tab_ffpkg_edit import build_ffpkg_edit_tab, open_ffpkg_path
from ui.tab_files import build_files_tab, _refresh_card as _fm_refresh_card


# mode -> (pill text, badge tone)
_MODE_PILL = {
    'ffpkg': ('FFPKG · in-place edit', 'info'),
    'exfat': ('exFAT · mounted edit', 'info'),
    'pfs':   ('PFS · workspace',       'warn'),
    'none':  ('No image open',              'neutral'),
}
_MODE_ICON = {'ffpkg': 'package', 'exfat': 'drive', 'pfs': 'compress',
              'none': 'circle'}


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

    # ── Page header: title + description; actions = mode badge, editor
    #    switcher and the unified Open (the page's primary action) ──
    hdr = PageHeader(
        parent, _('Edit'),
        _('Open a .ffpkg, .exfat, or PFS image — the editor '
          'is chosen automatically from the file.'))
    hdr.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

    pill_var = tk.StringVar(value=_MODE_PILL['none'][0])
    pill = Badge(hdr.actions, pill_var.get(), 'neutral', icon=False)
    pill.pack(side='left', padx=(0, px(12)))

    mode_var = tk.StringVar(value='ffpkg')
    seg = Segmented(hdr.actions,
                    [('ffpkg', 'ffpkg'), ('exfat', 'exFAT'), ('pfs', 'PFS')],
                    variable=mode_var, command=lambda m: _show_mode(m))
    seg.pack(side='left', padx=(0, px(12)))

    def _open():
        _edit_open(app, state, _show_mode, _set_pill, pfs_api)
    Button(hdr.actions, text=_('Open image…'), command=_open,
           variant='primary', icon='folder_open').pack(side='left')

    # ── Stacked editor panes ──
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    ffpkg_pane = tk.Frame(body, bg=COLORS['bg_1'])
    exfat_pane = tk.Frame(body, bg=COLORS['bg_1'])
    pfs_pane = tk.Frame(body, bg=COLORS['bg_1'])

    # Reuse the existing editors verbatim (no second editor is created);
    # embedded=True drops their own page headers — this page has one.
    build_ffpkg_edit_tab(ffpkg_pane, app, embedded=True)
    build_files_tab(exfat_pane, app, embedded=True)
    pfs_api = _build_pfs_workspace(pfs_pane, app, state)

    panes = {'ffpkg': ffpkg_pane, 'exfat': exfat_pane, 'pfs': pfs_pane}

    def _set_pill(mode_key):
        text, tone = _MODE_PILL.get(mode_key, _MODE_PILL['none'])
        pill_var.set(text)
        try:
            pill.set(text, tone)
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
        if mode_var.get() != mode:
            mode_var.set(mode)
        # keep the pill honest: only show an active edit-mode when one of
        # the editors actually holds an image
        if mode == 'pfs':
            _set_pill('pfs')

    # Expose a couple of hooks the PFS workspace uses for handoff.
    pfs_api['show_mode'] = _show_mode
    pfs_api['set_pill'] = _set_pill
    # Sub-mode activator (like the other pages' _*_subtab_activate), so
    # a route such as 'files' -> ('edit', 'exfat') can open a mode.
    app._edit_mode_activate = _show_mode

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
            app.after(800, lambda: _fm_refresh_card(app))
        except Exception:
            pass
    elif fmt in ('pfs', 'pfs_c'):
        show_mode('pfs')
        pfs_api['set_source'](p, compressed=(fmt == 'pfs_c'))
        set_pill('pfs')
    else:
        app_warning(app, _('Unsupported file type'),
            _('Supported image types: .ffpkg, .exfat, .ffpfs, .ffpfsc'))


# ─────────────────────────────────────────────────────────────────────────
# PFS workspace mode — unpack -> edit -> rebuild (never in place)
# ─────────────────────────────────────────────────────────────────────────
def _build_pfs_workspace(pane, app, state):
    api = {}
    BG1, BG2 = COLORS['bg_1'], COLORS['bg_2']
    wrap = tk.Frame(pane, bg=BG1)
    wrap.pack(fill='both', expand=True, padx=px(24), pady=(0, px(20)))

    # ── Workflow stepper (Select -> Unpack -> Edit -> Rebuild) ──
    steps_row = tk.Frame(wrap, bg=BG1)
    steps_row.pack(fill='x', pady=(0, px(16)))
    _STEP_DEFS = [
        ('1', _('Select PFS Image'), 'compress', _('Choose a .ffpfs or .ffpfsc.')),
        ('2', _('Unpack to Workspace'), 'folder_out',
         _('Extract contents to a folder.')),
        ('3', _('Edit Files'), 'file_edit',
         _('Edit files (or the nested image).')),
        ('4', _('Rebuild Image'), 'package',
         _('Rebuild a NEW image from Build.')),
    ]
    step_refs = []
    for i, (num, title, icon, sub) in enumerate(_STEP_DEFS):
        steps_row.columnconfigure(i, weight=1, uniform='step')
        sc = Rounded(steps_row, bg=BG2, border=COLORS['border_2'], radius=8,
                     padding=(px(14), px(12)))
        sc.grid(row=0, column=i, sticky='nsew',
                padx=(0 if i == 0 else px(12), 0))
        top = tk.Frame(sc, bg=BG2)
        top.pack(fill='x')
        ic = IconLabel(top, icon, 16, COLORS['fg_4'], BG2)
        ic.pack(side='left')
        eyebrow = tk.Label(top, text=(_('STEP ') + num).upper(),
                           font=FONTS['overline'], bg=BG2,
                           fg=COLORS['fg_4'], anchor='w')
        eyebrow.pack(side='left', padx=(px(8), 0))
        chk = IconLabel(top, 'circle', 16, COLORS['fg_5'], BG2)
        chk.pack(side='right')
        tl = tk.Label(sc, text=title, font=FONTS['body_strong'], bg=BG2,
                      fg=COLORS['fg_1'], anchor='w', justify='left')
        tl.pack(fill='x', pady=(px(10), 0))
        autowrap(tl)
        sl = tk.Label(sc, text=sub, font=FONTS['caption'], bg=BG2,
                      fg=COLORS['fg_3'], anchor='w', justify='left')
        sl.pack(fill='x', pady=(px(2), 0))
        autowrap(sl)
        sc.raise_decorations()
        step_refs.append({'card': sc, 'eyebrow': eyebrow, 'chk': chk,
                          'glyph': ic, 'title': tl})

    def _set_step(idx, st):
        # st: 'done' | 'active' | 'pending'
        r = step_refs[idx]
        if st == 'done':
            r['card'].set_style(border=COLORS['border_3'])
            r['chk'].set('success', COLORS['success'])
            r['glyph'].set(fg=COLORS['fg_2'])
            r['title'].config(fg=COLORS['fg_0'])
        elif st == 'active':
            r['card'].set_style(border=COLORS['accent'])
            r['chk'].set('play_circle', COLORS['accent_hi'])
            r['glyph'].set(fg=COLORS['fg_1'])
            r['title'].config(fg=COLORS['fg_0'])
        else:
            r['card'].set_style(border=COLORS['border_2'])
            r['chk'].set('circle', COLORS['fg_5'])
            r['glyph'].set(fg=COLORS['fg_4'])
            r['title'].config(fg=COLORS['fg_2'])
        r['card'].raise_decorations()

    def _steps(active_idx, done_upto):
        for i in range(4):
            if i < done_upto:
                _set_step(i, 'done')
            elif i == active_idx:
                _set_step(i, 'active')
            else:
                _set_step(i, 'pending')
    _steps(0, 0)

    # ── Workspace card ──
    card = Card(wrap, title=_('PFS workspace mode'),
                subtitle=_('PFS images are not edited in place. Unpack to a '
                           'workspace, edit the contents (or the nested '
                           'image), then rebuild to a NEW file from the Build '
                           'tab — the source is never modified.'))
    card.pack(fill='x')
    pad = card.body

    src_var = tk.StringVar(value='')
    ws_var = tk.StringVar(value='')

    def _browse_ws():
        d = filedialog.askdirectory(title=_('Select workspace folder'))
        if d:
            ws_var.set(d)

    src_block = field_block(pad, _('Source PFS image'), src_var)
    src_block.pack_configure(pady=0)
    try:
        src_block.entry.configure(state='readonly')
    except Exception:
        pass
    field_block(pad, _('Workspace folder'), ws_var,
                on_browse=_browse_ws, browse_text=_('Browse'))

    btn_row = tk.Frame(pad, bg=BG2)
    btn_row.pack(fill='x', pady=(px(16), 0))
    unpack_btn = Button(btn_row, text=_('Unpack to workspace'),
                        command=lambda: _unpack(), icon='play')
    unpack_btn.pack(side='left')

    status_var = tk.StringVar(value='')
    tk.Label(btn_row, textvariable=status_var, font=FONTS['body'],
             bg=BG2, fg=COLORS['fg_2'], anchor='w').pack(
                 side='left', fill='x', expand=True, padx=(px(12), 0))

    # action area — repopulated after a successful unpack
    actions = tk.Frame(pad, bg=BG2)
    actions.pack(fill='x', pady=(px(8), 0))

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

    def _action_note(text):
        lbl = tk.Label(actions, text=text, font=FONTS['caption'], bg=BG2,
                       fg=COLORS['fg_3'], anchor='w', justify='left')
        lbl.pack(fill='x', pady=(px(8), px(6)))
        autowrap(lbl)

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
            _action_note(_('Nested .ffpkg found — edit it directly:'))
            Button(actions, text=_('Edit nested .ffpkg here'),
                   command=lambda np=nested: _handoff_ffpkg(np),
                   icon='package').pack(side='left')
        elif kind == 'exfat':
            _action_note(_('Nested .exfat found — mount and edit it:'))
            Button(actions, text=_('Edit nested .exfat here'),
                   command=lambda np=nested: _handoff_exfat(np),
                   icon='drive').pack(side='left')
        else:
            msg = (_('Unpacked the nested raw PFS image (pfs_image.dat). '
                     'It cannot be edited directly here.')
                   if kind == 'raw'
                   else _('Unpacked the game files. Edit them on disk, then '
                          'rebuild a new .ffpfsc from the Build tab.'))
            _action_note(msg)
            Button(actions, text=_('Rebuild in Build tab'),
                   command=_go_build, icon='build').pack(side='left')

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
            app.after(800, lambda: _fm_refresh_card(app))
        except Exception:
            pass

    def _unpack():
        src = src_var.get().strip()
        ws = ws_var.get().strip()
        if not src or not os.path.isfile(src):
            app_warning(app, _('Missing'),
                _('No PFS image selected.'))
            return
        if not ws or not os.path.isdir(ws):
            app_warning(app, _('Missing'),
                _('Pick a workspace folder.'))
            return
        stem = os.path.splitext(os.path.basename(src))[0]
        dest = os.path.normpath(os.path.join(ws, stem))
        overwrite = os.path.exists(dest)
        if overwrite and not app_confirm(app, _('Folder exists'),
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
