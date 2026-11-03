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
import re
import time
import threading
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from tkinter import filedialog, messagebox, ttk

from tkinter_theme import COLORS, FONTS
from ui.shared.app_dialog import (
    app_info, app_warning, app_error, app_confirm, app_choose3,
)
from ui.shared.page_head import (
    make_themed_button, info_banner, page_head, field_block)
from ui.shared.scroll import attach_scroll

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
    # Farewell Edition: built, but verification flagged it - the image is
    # kept for the user to check (issue #86).
    'warn':    ('CHECK',    'warn'),
}

_DARK_TEXT = COLORS['on_accent']
_SEL_BG    = COLORS['accent_08']


def _dash_card(parent, title, subtitle=None,
               head_pady=(16, 2), body_pady=(10, 18)):
    """A design-system card with a title + optional subtitle.
    Returns (card, body)."""
    from ui.shared.widgets import Card
    card = Card(parent, title=title, subtitle=subtitle)
    return card, card.body


def _hero_stat(parent, label_text, var):
    """Caption label above a value (Source card facts)."""
    bg = COLORS['bg_2']
    box = tk.Frame(parent, bg=bg)
    tk.Label(box, text=label_text, font=FONTS['caption'], bg=bg,
             fg=COLORS['fg_3'], anchor='w').pack(anchor='w')
    val = tk.Label(box, textvariable=var, font=FONTS['body_strong'], bg=bg,
                   fg=COLORS['fg_0'], anchor='w')
    val.pack(anchor='w', pady=(2, 0))
    return box, val


def _strip_cell(parent, label_text, var, stacked=False):
    """One summary row: caption label left, value right (or stacked).
    Returns (cell, value label) so callers can recolour the value."""
    bg = COLORS['bg_2']
    cell = tk.Frame(parent, bg=bg)
    lbl = tk.Label(cell, text=label_text, font=FONTS['body'], bg=bg,
                   fg=COLORS['fg_3'], anchor='w')
    val = tk.Label(cell, textvariable=var, font=FONTS['body_strong'], bg=bg,
                   fg=COLORS['fg_0'], anchor='e' if not stacked else 'w',
                   justify='left')
    if stacked:
        lbl.pack(fill='x')
        val.pack(fill='x', pady=(2, 0))
        from ui.shared.widgets import autowrap
        autowrap(val)
    else:
        lbl.pack(side='left')
        val.pack(side='right')
    return cell, val


def _toast(app, title, line1='', line2='', ok=True):
    """Bottom-right auto-dismissing notification (design system › Toast)."""
    try:
        from ui.shared.widgets import Toasts
        mgr = getattr(app, '_xb_toasts', None)
        if mgr is None:
            mgr = app._xb_toasts = Toasts(app)
        msg = '\n'.join(x for x in (line1, line2) if x)
        mgr.show(title, msg, kind='success' if ok else 'warn', timeout=5000)
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
    inner.bind('<Configure>', lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e: canvas.itemconfig(inner_id, width=e.width))
    attach_scroll(canvas)
    src_var = tk.StringVar()
    outdir_var = tk.StringVar(value=str(app._settings.get('output_dir') or ''))
    _out_user_choice = {'set': bool(str(app._settings.get('output_dir') or '').strip())}
    _out_programmatic = {'on': False}
    def _set_outdir_programmatic(value):
        """Set Output without marking it as an explicit user choice."""
        _out_programmatic['on'] = True
        try:
            outdir_var.set(value)
        finally:
            _out_programmatic['on'] = False
    name_var = tk.StringVar()
    out_type = tk.StringVar(value='exfat')
    pfs_via = tk.StringVar(value=str(app._settings.get('pfs_intermediate_via', 'exfat')))
    pfs_src_mode = tk.StringVar(value='folder')
    pfs_backend = tk.StringVar(value=str(app._settings.get('pfs_backend', 'lazy')).strip().lower() or 'lazy')
    pfs_zlib_backend = tk.StringVar(value=str(app._settings.get('pfs_lazy_zlib_backend', 'zlib')).strip().lower() or 'zlib')
    name_preset = tk.StringVar(value=str(app._settings.get('name_preset', 'ppsa_title_ver') or 'ppsa_title_ver'))
    name_template = tk.StringVar(value=str(app._settings.get('name_custom_template', '%TITLE% (%VERSION%) (%FIRMWARE%) (%PPSA%)') or '%TITLE% (%VERSION%) (%FIRMWARE%) (%PPSA%)'))
    name_fw_override = tk.StringVar(value='auto')
    tempdir_var = tk.StringVar(value=str(app._settings.get('temp_dir') or ''))
    det_title_var = tk.StringVar(value='No game dump selected')
    det_id_var = tk.StringVar(value='')
    hero_ver_var = tk.StringVar(value='—')
    hero_size_var = tk.StringVar(value='—')
    hero_free_var = tk.StringVar(value='—')
    stat_jobs_var = tk.StringVar(value='No queued jobs')
    stat_estsize_var = tk.StringVar(value='—')
    stat_needed_var = tk.StringVar(value='—')
    stat_space_var = tk.StringVar(value='—')
    stat_comp_var = tk.StringVar(value='')
    stat_disk_var = tk.StringVar(value='')
    space_state = {'ok': True, 'out_need': 0, 'tmp_need': 0, 'out_free': None, 'tmp_free': None, 'same_drive': False, 'out_drive': '', 'tmp_drive': '', 'detail': ''}
    _space_gen = {'n': 0}
    sum_title_var = tk.StringVar(value='BUILD COMPLETE')
    sum_jobs_var = tk.StringVar(value='')
    uq_status = tk.StringVar(value='')
    uq_step_var = tk.StringVar(value='')
    uq_pct_var = tk.StringVar(value='')
    uq_eta_var = tk.StringVar(value='')
    uq_count = tk.StringVar(value='')
    detected = {}
    cover_ref = {'img': None}
    tile_refs = {}
    queue = []
    uq_state = {'sel': {}, 'running': False, 'current': None, 'cancel': False}
    def _choose_game_root(candidates):
        """Modal list for the rare multi-root case. Returns the chosen path
        or None if cancelled. Presentation only — no build logic."""
        try:
            from tkinter_theme import px
            from ui.shared.widgets import Button as _B, IconLabel as _IL
            dlg = tk.Toplevel(parent)
            dlg.title('Multiple game roots found')
            dlg.configure(bg=COLORS['bg_2'])
            dlg.transient(parent.winfo_toplevel())
            dlg.grab_set()
            body = tk.Frame(dlg, bg=COLORS['bg_2'])
            body.pack(fill='both', expand=True, padx=px(24), pady=(px(22), px(16)))
            head = tk.Frame(body, bg=COLORS['bg_2'])
            head.pack(fill='x')
            _IL(head, 'folder_tree', 20, COLORS['accent_hi'], COLORS['bg_2']).pack(side='left', padx=(0, px(10)))
            tk.Label(head, text='Pick the game root', bg=COLORS['bg_2'], fg=COLORS['fg_0'], font=FONTS['title']).pack(side='left')
            tk.Label(body, text='Several folders contain eboot.bin. Pick the game root to build from:', bg=COLORS['bg_2'], fg=COLORS['fg_2'], font=FONTS['body'], justify='left').pack(anchor='w', pady=(px(10), px(10)))
            chosen = {'path': None}
            lb = tk.Listbox(body, width=80, height=min(12, len(candidates)), bg=COLORS['bg_0'], fg=COLORS['fg_1'], font=FONTS['code'], selectbackground=COLORS['accent_lo'], selectforeground=COLORS['fg_0'], highlightthickness=1, highlightbackground=COLORS['border_2'], highlightcolor=COLORS['focus'], bd=0, activestyle='none')
            for c in candidates:
                lb.insert('end', '  ' + c)
            lb.select_set(0)
            lb.pack(fill='both', expand=True)
            tk.Frame(dlg, bg=COLORS['border_2'], height=1).pack(fill='x')
            btns = tk.Frame(dlg, bg=COLORS['bg_1'])
            btns.pack(fill='x')
            _bar = tk.Frame(btns, bg=COLORS['bg_1'])
            _bar.pack(fill='x', padx=px(18), pady=px(10))
            def _ok():
                sel = lb.curselection()
                if sel:
                    chosen['path'] = candidates[sel[0]]
                dlg.destroy()
            def _cancel():
                chosen['path'] = None
                dlg.destroy()
            _B(_bar, text='Use this folder', variant='primary', command=_ok, surface=COLORS['bg_1']).pack(side='right')
            _B(_bar, text='Cancel', command=_cancel, surface=COLORS['bg_1']).pack(side='right', padx=(0, px(4)))
            dlg.bind('<Return>', lambda _e: _ok())
            dlg.bind('<Escape>', lambda _e: _cancel())
            parent.wait_window(dlg)
            return chosen['path']
        except Exception:
            return
    def _browse_src():
        if out_type.get() == 'pfs' and pfs_src_mode.get() == 'image':
            p = filedialog.askopenfilename(title='Select an existing .exfat / .ffpkg image', filetypes=[*(('Game images', '*.exfat *.ffpkg'), ('exFAT images', '*.exfat'), ('ffpkg images', '*.ffpkg'), ('All files', '*.*'))])
            if p:
                src_var.set(p)
            return
        p = filedialog.askdirectory(title='Select game dump folder')
        if not p:
            return
        try:
            from exfat_builder import resolve_game_root_deep
        except Exception:
            resolve_game_root_deep = None
        _log_line = getattr(app, '_log', lambda *_a, **_k: None)
        _log_line('[DETECT] Selected source: %s\n' % p)
        if resolve_game_root_deep is None:
            src_var.set(p)
            return
        _log_line('[DETECT] Scanning for eboot.bin…\n')
        res = resolve_game_root_deep(p, log=_log_line)
        cands = res.get('candidates', [])
        if res.get('root'):
            root = res['root']
            if os.path.normpath(root) != os.path.normpath(p):
                _log_line('[DETECT] Using resolved game root: %s\n' % root)
                try:
                    _toast(app, 'Game root found', 'Found game root inside the selected folder:', root, ok=True)
                except Exception:
                    pass
            src_var.set(root)
            return
        if len(cands) > 1:
            _log_line('[DETECT] Multiple possible game roots found (%d). Please pick the exact one.\n' % len(cands))
            choice = _choose_game_root(cands)
            if choice:
                _log_line('[DETECT] Using selected game root: %s\n' % choice)
                src_var.set(choice)
                return
            src_var.set(p)
            return
        if res.get('truncated'):
            _log_line('[DETECT] No eboot.bin found before the scan limit; using the selected folder as-is.\n')
        else:
            _log_line('[DETECT] No eboot.bin found beneath the selected folder; using it as-is.\n')
        src_var.set(p)
    def _persist_setting(key, value):
        """v3.6.6: write a single settings key to disk immediately, only when
        it actually changed. Used by the Output / Temp browse handlers so the
        chosen folder survives an app restart (previously Output was never
        persisted and Temp only at build start)."""
        try:
            if (app._settings.get(key) or '') != (value or ''):
                app._settings[key] = value
                from exfat_builder import save_settings
                save_settings(app._settings)
        except Exception:
            return
    def _browse_out():
        p = filedialog.askdirectory(title='Select output folder')
        if p:
            _out_user_choice['set'] = True
            outdir_var.set(p)
            _persist_setting('output_dir', p)
    def _browse_temp():
        p = filedialog.askdirectory(title='Select temp / spool folder')
        if p:
            tempdir_var.set(p)
            _persist_setting('temp_dir', p)
    # ── Page layout (design system) ─────────────────────────────────────
    # Page header, then two columns: Source / Output format / Output on the
    # left, Build summary + actions and the advanced panels on the right.
    # The progress card, queue and completion summary span the full width.
    from tkinter_theme import px
    from ui.shared.widgets import (
        PageHeader, Card, Badge, Dropdown, Expander, RoundedBar, StepIcon,
        CoverLabel, IconLabel, Divider, Rounded, Button, IconButton,
        autowrap, TextField)
    from ui.icons import icon_image
    BG1, BG2, BG3 = COLORS['bg_1'], COLORS['bg_2'], COLORS['bg_3']
    from ui.shared.page_head import _tr
    page_hdr = PageHeader(
        inner, _tr('Build'),
        'Pack a game dump into an exFAT, ffpkg or compressed PFS image. '
        'Jobs of different formats can share one queue.')
    page_hdr.pack(fill='x', padx=px(24), pady=(px(20), px(16)))
    cols = tk.Frame(inner, bg=BG1)
    cols.pack(fill='x', padx=px(24), pady=(0, px(16)))
    cols.columnconfigure(0, weight=7, uniform='bcols')
    cols.columnconfigure(1, weight=5, uniform='bcols')
    left = tk.Frame(cols, bg=BG1)
    left.grid(row=0, column=0, sticky='nsew', padx=(0, px(8)))
    right = tk.Frame(cols, bg=BG1)
    right.grid(row=0, column=1, sticky='nsew', padx=(px(8), 0))
    _cols_mode = {'stacked': None}

    def _reflow_cols(e=None):
        # Narrow windows: stack the right column under the left one.
        stacked = cols.winfo_width() < px(900)
        if stacked == _cols_mode['stacked']:
            return
        _cols_mode['stacked'] = stacked
        if stacked:
            cols.columnconfigure(1, weight=0, uniform='')
            cols.columnconfigure(0, weight=1, uniform='')
            left.grid_configure(row=0, column=0, columnspan=2, padx=0)
            right.grid_configure(row=1, column=0, columnspan=2, padx=0,
                                 pady=(px(16), 0))
        else:
            cols.columnconfigure(0, weight=7, uniform='bcols')
            cols.columnconfigure(1, weight=5, uniform='bcols')
            left.grid_configure(row=0, column=0, columnspan=1, padx=(0, px(8)))
            right.grid_configure(row=0, column=1, columnspan=1,
                                 padx=(px(8), 0), pady=0)
    cols.bind('<Configure>', _reflow_cols, add='+')

    # ── Source ──
    hero = Card(left, title='Source',
                subtitle='The game dump folder that contains eboot.bin.')
    hero.pack(fill='x')
    Button(hero.head_actions, text='Browse dump', icon='folder_open',
           command=(lambda: _browse_src()), surface=BG2).pack()
    hero_in = tk.Frame(hero.body, bg=BG2)
    hero_in.pack(fill='x')
    cover_lbl = CoverLabel(hero_in, box=112, placeholder='game')
    cover_lbl.pack(side='left', anchor='n', padx=(0, px(16)))
    hcol = tk.Frame(hero_in, bg=BG2)
    hcol.pack(side='left', fill='both', expand=True)
    autowrap(tk.Label(hcol, textvariable=det_title_var, font=FONTS['subtitle'],
                      bg=BG2, fg=COLORS['fg_0'], anchor='w', justify='left'))
    hcol.winfo_children()[-1].pack(fill='x')
    tk.Label(hcol, textvariable=det_id_var, font=FONTS['code'], bg=BG2,
             fg=COLORS['fg_3'], anchor='w').pack(fill='x', pady=(px(2), px(12)))
    stat_row = tk.Frame(hcol, bg=BG2)
    stat_row.pack(fill='x')
    for c in range(3):
        stat_row.columnconfigure(c, weight=1, uniform='hs')
    b1, _ = _hero_stat(stat_row, 'Version', hero_ver_var)
    b1.grid(row=0, column=0, sticky='w')
    b2, _ = _hero_stat(stat_row, 'Dump size', hero_size_var)
    b2.grid(row=0, column=1, sticky='w')
    b3, free_val = _hero_stat(stat_row, 'Output free', hero_free_var)
    b3.grid(row=0, column=2, sticky='w')
    hero_bottom = tk.Frame(hcol, bg=BG2)
    hero_bottom.pack(fill='x', pady=(px(12), 0))
    ready_badge = Badge(hero_bottom, 'Ready to build', 'success')
    ready_hint = tk.Label(hero_bottom, text='', font=FONTS['caption'], bg=BG2,
                          fg=COLORS['fg_3'], anchor='w', justify='left')

    # ── Output format ──
    fmt_card, fmt_body = _dash_card(left, 'Output format',
                                    'Pick a format, then add the dump to the queue.')
    fmt_card.pack(fill='x', pady=(px(16), 0))
    fmt_row = tk.Frame(fmt_body, bg=BG2)
    fmt_row.pack(fill='x')
    for c in range(3):
        fmt_row.columnconfigure(c, weight=1, uniform='fmt')

    def _restyle_tiles():
        for k, ref in tile_refs.items():
            sel = (out_type.get() == k)
            bg = COLORS['accent_08'] if sel else BG3
            ref['card'].set_style(bg=bg, border=COLORS['accent'] if sel else COLORS['border_3'])
            for w in ref['labels']:
                w.config(bg=bg)
            ref['labels'][0].config(image=icon_image(
                ref['icon'], 20, COLORS['accent_hi'] if sel else COLORS['fg_2'], bg))
            ref['labels'][1].config(fg=COLORS['fg_0'])
            ref['check'].config(bg=bg, image=icon_image('success', 16, COLORS['accent_hi'], bg))
            if sel:
                ref['check'].place(relx=1.0, x=-px(10), y=px(10), anchor='ne')
            else:
                ref['check'].place_forget()
            ref['card'].raise_decorations()

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

    def _make_type_card(col, key, icon, title, sub, recommended=False, tip=''):
        card = Rounded(fmt_row, bg=BG3, border=COLORS['border_3'], radius=8,
                       padding=(px(14), px(12)), surface=BG2, cursor='hand2')
        card.grid(row=0, column=col, sticky='nsew',
                  padx=(0 if col == 0 else px(8), 0))
        labels = []
        ic = tk.Label(card, bg=BG3, bd=0, anchor='w')
        ic.pack(anchor='w')
        labels.append(ic)
        nm = tk.Label(card, text=title, font=FONTS['body_strong'], bg=BG3,
                      fg=COLORS['fg_0'], anchor='w')
        nm.pack(fill='x', pady=(px(8), 0))
        labels.append(nm)
        sbl = tk.Label(card, text=sub, font=FONTS['caption'], bg=BG3,
                       fg=COLORS['fg_3'], anchor='w', justify='left')
        sbl.pack(fill='x', pady=(px(2), 0))
        autowrap(sbl)
        labels.append(sbl)
        if recommended:
            Badge(card, 'Recommended', 'success', surface=BG3).pack(anchor='w', pady=(px(8), 0))
        check = tk.Label(card, bg=BG3, bd=0)
        for w in [card] + labels:
            w.bind('<Button-1>', lambda e, kk=key: _pick_type(kk))
        if tip:
            try:
                from exfat_builder import Tooltip
                for w in [card] + labels:
                    Tooltip(w, tip)
            except Exception:
                pass
        tile_refs[key] = {'card': card, 'labels': labels, 'check': check, 'icon': icon}
    _make_type_card(0, 'exfat', 'drive', 'exFAT', 'Recommended — mountable image for most setups', recommended=True, tip='Recommended for most users. Easy to mount and widely supported.')
    _make_type_card(1, 'ffpkg', 'package', 'ffpkg', 'Package-style image', tip='Package-style image format.')
    _make_type_card(2, 'pfs', 'compress', 'PFS', 'Compressed image — smaller, slower to build', tip='Compressed image format. Smaller size, slower build.')
    tk.Label(fmt_body, text='Not sure? exFAT is the safe default for most games.', font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w').pack(fill='x', pady=(px(10), 0))
    adv_state = {'open': False}
    adv_disc_header = tk.Label(fmt_body, text=' Advanced options', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2'], cursor='hand2', anchor='w', compound='left', image=icon_image('chevron_right', 16, COLORS['fg_3'], BG2))
    via_frame = tk.Frame(fmt_body, bg=BG2)
    tk.Label(via_frame, text='Intermediate format', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2'], anchor='w').pack(anchor='w')

    def _save_pfs_via():
        # v3.6.3: persist the intermediate choice so it survives restart.
        try:
            app._settings['pfs_intermediate_via'] = pfs_via.get()
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            pass
    _via_radios = tk.Frame(via_frame, bg=BG2)
    _via_radios.pack(anchor='w', pady=(px(4), 0))
    for val, lab in ('exfat', 'exFAT (recommended)'), ('ffpkg', 'ffpkg'):
        XRadio(_via_radios, text=lab, value=val, variable=pfs_via, command=(lambda: (_save_pfs_via(), _refresh_strip(), _update_space()))).pack(side='left', padx=(0, px(16)))
    autowrap(tk.Label(via_frame, text='Determines the temporary format used before PFS compression. Most users should leave this set to exFAT.', font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left'))
    via_frame.winfo_children()[-1].pack(fill='x', pady=(px(4), 0))

    def _toggle_advanced():
        adv_state['open'] = not adv_state['open']
        adv_disc_header.config(image=icon_image(
            'chevron_down' if adv_state['open'] else 'chevron_right', 16,
            COLORS['fg_3'], BG2))
        _refresh_via()
    adv_disc_header.bind('<Button-1>', lambda e: _toggle_advanced())
    pfs_mode_frame = tk.Frame(fmt_body, bg=BG2)
    tk.Label(pfs_mode_frame, text='PFS source', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2']).pack(side='left', padx=(0, px(14)))
    for _val, _lab in ('folder', 'Game dump folder'), ('image', 'Existing .exfat / .ffpkg'):
        XRadio(pfs_mode_frame, text=_lab, value=_val, variable=pfs_src_mode, command=(lambda: _on_pfs_mode_change())).pack(side='left', padx=(0, px(16)))
    pfs_backend_frame = tk.Frame(fmt_body, bg=BG2)
    tk.Label(pfs_backend_frame, text='PFS backend', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2']).pack(side='left', padx=(0, px(14)))

    def _save_pfs_backend():
        try:
            val = (pfs_backend.get() or 'lazy').strip().lower()
            if val not in ('lazy', 'mkpfs'):
                val = 'lazy'
            app._settings['pfs_backend'] = val
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            pass
        try:
            _refresh_isa_visibility()
        except NameError:
            return

    def _lazy_label():
        try:
            from ui.experimental_mkpfs import _dir_has_main_file
            ov = (app._settings.get('experimental_mkpfs_folder', '') or '').strip()
            if ov and _dir_has_main_file(ov):
                return 'Lazy_MkPFS by Nazky (Override folder)'
        except Exception:
            pass
        return 'Lazy_MkPFS by Nazky (Bundled)'

    def _mkpfs_label():
        try:
            from ui.mkpfs_runner import mkpfs_version
            ver = mkpfs_version()
            if ver and ver != '(installed)':
                return 'MkPFS by PSBrew (%s) — Experimental' % ver
        except Exception:
            pass
        return 'MkPFS by PSBrew (native) — Experimental'
    _lazy_radio = {'w': None}
    for _val, _lab in ('lazy', _lazy_label()), ('mkpfs', _mkpfs_label()):
        _rb = XRadio(pfs_backend_frame, text=_lab, value=_val, variable=pfs_backend, command=_save_pfs_backend)
        _rb.pack(side='left', padx=(0, px(16)))
        if _val == 'lazy':
            _lazy_radio['w'] = _rb

    def _refresh_lazy_label():
        try:
            if _lazy_radio['w'] is not None:
                _lazy_radio['w'].config(text=_lazy_label())
        except Exception:
            return
    pfs_backend_note = tk.Label(fmt_body, text='Two backends are available. Lazy_MkPFS is bundled and used by default. MkPFS (PSBrew) is also bundled and EXPERIMENTAL — it can do OSFMount-free extract/verify, but validate it on your own images/console first. If the chosen backend fails, the tool can fall back to the other.', font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left')
    autowrap(pfs_backend_note)
    pfs_isa_frame = tk.Frame(fmt_body, bg=BG2)
    _isa_var = tk.BooleanVar(value=pfs_zlib_backend.get() == 'isa-l')

    def _save_isa():
        try:
            val = 'isa-l' if _isa_var.get() else 'zlib'
            pfs_zlib_backend.set(val)
            app._settings['pfs_lazy_zlib_backend'] = val
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            return
    XCheck(pfs_isa_frame, text='ISA-L acceleration — experimental', variable=_isa_var, command=_save_isa).pack(side='left')
    pfs_isa_note = tk.Label(fmt_body, text='Faster compression backend for Lazy_MkPFS. Uses native code and supports compression levels 0–3 only (higher levels are clamped to 3). Default remains safe zlib until PS5 mount testing is confirmed.', font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left')
    autowrap(pfs_isa_note)

    def _refresh_isa_visibility():
        try:
            pfs_isa_frame.pack_forget()
            pfs_isa_note.pack_forget()
        except Exception:
            return
    pfs_xref_lbl = tk.Label(fmt_body, text='Converting between exFAT and ffpkg? Use the Convert tab.', font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w')

    def _refresh_via():
        if out_type.get() == 'pfs':
            pfs_mode_frame.pack(anchor='w', pady=(px(14), 0))
            pfs_backend_frame.pack_forget()
            pfs_backend_note.pack_forget()
            _refresh_lazy_label()
            _refresh_isa_visibility()
        else:
            pfs_mode_frame.pack_forget()
            pfs_backend_frame.pack_forget()
            pfs_backend_note.pack_forget()
        _show_adv = out_type.get() == 'pfs' and pfs_src_mode.get() == 'folder'
        if _show_adv:
            adv_disc_header.pack(anchor='w', pady=(px(10), 0))
            if adv_state['open']:
                via_frame.pack(fill='x', pady=(px(6), 0), padx=(px(22), 0))
            else:
                via_frame.pack_forget()
        else:
            adv_disc_header.pack_forget()
            via_frame.pack_forget()
        if out_type.get() == 'pfs' and pfs_src_mode.get() == 'image':
            pfs_xref_lbl.pack(anchor='w', pady=(px(8), 0))
        else:
            pfs_xref_lbl.pack_forget()
        try:
            _refresh_apo_groups()
        except NameError:
            return

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
                             else '🎮')
        except Exception:
            pass
        _refresh_via()
        _refresh_strip()
        _refresh_ready()
        _update_space()
    img_size_mode = tk.StringVar(value='auto')
    img_size_gb = tk.StringVar(value='')
    size_frame = tk.Frame(fmt_body, bg=BG2)
    tk.Label(size_frame, text='exFAT image size', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2']).pack(side='left', padx=(0, px(14)))
    for val, lab in ('auto', 'Auto (smallest)'), ('custom', 'Custom'):
        XRadio(size_frame, text=lab, value=val, variable=img_size_mode).pack(side='left', padx=(0, px(10)))
    TextField(size_frame, textvariable=img_size_gb, width=6, justify='center').pack(side='left')
    tk.Label(size_frame, text='GB', font=FONTS['body'], bg=BG2, fg=COLORS['fg_3']).pack(side='left', padx=(px(6), 0))

    def _refresh_size_row():
        if out_type.get() == 'exfat':
            size_frame.pack(anchor='w', pady=(px(14), 0))
        else:
            size_frame.pack_forget()

    # ── Output (folders + naming) ──
    cfg_card, cfg_body = _dash_card(left, 'Output',
                                    'Where the image is written and what it is called.')
    cfg_card.pack(fill='x', pady=(px(16), 0))
    _out_block = field_block(cfg_body, 'Output folder', var=outdir_var, on_browse=_browse_out, hint='where the image will be written')
    _out_block.pack_configure(pady=0)
    _temp_block = field_block(cfg_body, 'Temp folder (optional)', var=tempdir_var, on_browse=_browse_temp, hint='redirects the build spool off C: — applied on build')
    autowrap(tk.Label(_temp_block, text='For existing .exfat / .ffpkg → .ffpfsc builds the Temp folder is also the staging location. Choose a folder on an NTFS drive for the fastest staging — exFAT and FAT32 can’t hard-link, so the tool may have to copy the full image before packing.', font=FONTS['caption'], justify='left', anchor='w', bg=BG2, fg=COLORS['fg_3']))
    _temp_block.winfo_children()[-1].pack(fill='x', pady=(px(6), 0))
    Divider(cfg_body, color=COLORS['border_1']).pack(fill='x', pady=(px(16), px(4)))
    name_body = cfg_body
    _PRESETS = [('ppsa', 'PPSA only'), ('ppsa_title', 'PPSA + Title'),
                ('ppsa_title_ver', 'PPSA + Title + Version'),
                ('ppsa_title_ver_fw', 'PPSA + Title + Version + Firmware'),
                ('custom', 'Custom template')]
    _preset_lbl = dict(_PRESETS)
    _preset_key = {v: k for k, v in _PRESETS}
    preset_row = tk.Frame(name_body, bg=BG2)
    preset_row.pack(fill='x', pady=(px(8), 0))
    preset_row.columnconfigure(0, weight=3, uniform='np')
    preset_row.columnconfigure(1, weight=2, uniform='np')
    _pcol = tk.Frame(preset_row, bg=BG2)
    _pcol.grid(row=0, column=0, sticky='ew', padx=(0, px(12)))
    tk.Label(_pcol, text='Name preset', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2'], anchor='w').pack(fill='x')
    _preset_disp = tk.StringVar(value=_preset_lbl.get(name_preset.get(), _PRESETS[2][1]))
    _preset_dd = Dropdown(_pcol, textvariable=_preset_disp, values=[p[1] for p in _PRESETS])
    _preset_dd.pack(fill='x', pady=(px(4), 0))

    def _preset_pick(_e=None):
        name_preset.set(_preset_key.get(_preset_disp.get(), 'ppsa_title_ver'))
        _on_preset_change()
    _preset_dd.bind('<<ComboboxSelected>>', _preset_pick)
    name_preset.trace_add('write', lambda *_a: _preset_disp.set(_preset_lbl.get(name_preset.get(), _preset_disp.get())))
    fw_row = tk.Frame(preset_row, bg=BG2)
    fw_row.grid(row=0, column=1, sticky='ew')
    fw_lbl = tk.Label(fw_row, text='Firmware', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2'], anchor='w')
    fw_lbl.pack(fill='x')
    _fw_choices = ['auto'] + [str(_n) for _n in range(1, 14)]

    def _fw_label(v):
        return 'Auto (detected)' if v == 'auto' else 'FW' + v
    _fw_disp = tk.StringVar(value=_fw_label(name_fw_override.get()))
    fw_menu = Dropdown(fw_row, textvariable=_fw_disp, values=[_fw_label(c) for c in _fw_choices])
    fw_menu.pack(fill='x', pady=(px(4), 0))

    def _fw_pick(_e=None):
        lab = _fw_disp.get()
        name_fw_override.set('auto' if lab.startswith('Auto') else lab[2:])
        _gen_name()
    fw_menu.bind('<<ComboboxSelected>>', _fw_pick)
    name_fw_override.trace_add('write', lambda *_a: _fw_disp.set(_fw_label(name_fw_override.get())))
    tmpl_row = tk.Frame(name_body, bg=BG2)
    tmpl_lbl = tk.Label(tmpl_row, text='Template', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2'], anchor='w')
    tmpl_lbl.pack(fill='x')
    tmpl_entry = TextField(tmpl_row, textvariable=name_template, mono=True)
    tmpl_entry.pack(fill='x', pady=(px(4), 0))
    tmpl_hint = tk.Label(name_body, text='Tokens: %TITLE%  %PPSA%  %VERSION%  %FIRMWARE%  %TYPE%  %DATE%  ·  empty ( ) are removed automatically', font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left')
    autowrap(tmpl_hint)

    def _show_tmpl(show):
        if show:
            tmpl_row.pack(fill='x', pady=(px(12), 0), before=_name_block)
            tmpl_hint.pack(fill='x', pady=(px(6), 0), before=_name_block)
            return
        tmpl_row.pack_forget()
        tmpl_hint.pack_forget()

    def _on_template_write(*_a):
        try:
            _persist_setting('name_custom_template', name_template.get())
        except Exception:
            pass
        try:
            _gen_name()
        except Exception:
            return
    name_template.trace_add('write', _on_template_write)

    def _fw_picker_sync_state():
        try:
            is_fw = name_preset.get() == 'ppsa_title_ver_fw'
            fw_menu.configure(state='readonly' if is_fw else 'disabled')
            fw_lbl.configure(fg=COLORS['fg_2'] if is_fw else COLORS['fg_5'])
        except Exception:
            return

    def _on_preset_change():
        _fw_picker_sync_state()
        try:
            _show_tmpl(name_preset.get() == 'custom')
        except Exception:
            pass
        try:
            _persist_setting('name_preset', name_preset.get())
        except Exception:
            pass
        try:
            _gen_name()
        except Exception:
            return
    _name_block = field_block(name_body, 'Output name', var=name_var, hint='auto-filled from the preset; you can still edit it')
    _fw_picker_sync_state()
    _show_tmpl(name_preset.get() == 'custom')

    # ── Right column: build summary + actions ──
    sum_card = Card(right, title='Build summary',
                    subtitle='Space estimates update as you choose a source and output.')
    sum_card.pack(fill='x')
    strip = tk.Frame(sum_card.body, bg=BG2)
    strip.pack(fill='x')
    strip.columnconfigure(0, weight=1)
    jobs_cell, jobs_val = _strip_cell(strip, 'Queue', stat_jobs_var)
    jobs_cell.grid(row=0, column=0, sticky='ew', pady=(0, px(8)))
    jobs_val.config(fg=COLORS['fg_3'])
    _strip_cell(strip, 'Expected output', stat_estsize_var)[0].grid(row=1, column=0, sticky='ew', pady=(0, px(8)))
    needed_cell, needed_val = _strip_cell(strip, 'Output needed', stat_needed_var)
    needed_cell.grid(row=2, column=0, sticky='ew', pady=(0, px(8)))
    space_cell, space_val = _strip_cell(strip, 'Output space', stat_space_var)
    space_cell.grid(row=3, column=0, sticky='ew')
    disk_cell, disk_val = _strip_cell(strip, 'Temp drive', stat_disk_var, stacked=True)
    disk_cell.grid(row=4, column=0, sticky='ew', pady=(px(10), 0))
    disk_cell.grid_remove()
    comp_cell = _strip_cell(strip, 'Compression mode', stat_comp_var, stacked=True)[0]
    comp_cell.grid(row=5, column=0, sticky='ew', pady=(px(10), 0))
    comp_cell.grid_remove()
    Divider(sum_card.body, color=COLORS['border_1']).pack(fill='x', pady=(px(14), px(12)))
    btn_row = tk.Frame(sum_card.body, bg=BG2)
    btn_row.pack(fill='x')
    btn_build = Button(btn_row, text='Build image', icon='play', variant='primary', command=(lambda: _uq_build_all()), surface=BG2)
    btn_build.pack(side='right')

    def _refresh_build_btn_label():
        try:
            n = len(queue)
            label = 'Build queue (%d)' % n if n else 'Build image'
            btn_build.config(text=label)
        except Exception:
            return
    btn_add = Button(btn_row, text='Add to queue', icon='plus', command=(lambda: _uq_add()), surface=BG2)
    btn_add.pack(side='right', padx=(0, px(6)))
    btn_cancel = Button(btn_row, text='Cancel build', icon='stop', variant='danger', command=(lambda: _uq_cancel()), surface=BG2)

    def _uq_show_cancel(show):
        try:
            if show:
                btn_cancel.config(state='normal', text='Cancel build')
                btn_cancel.pack(side='left')
                return
            btn_cancel.pack_forget()
        except Exception:
            return

    def _uq_cancel():
        if not uq_state.get('running') or uq_state.get('cancel'):
            return
        if not app_confirm(app, 'Cancel build', 'Stop the build queue?\n\n• PFS compression stops right away.\n• An exFAT/ffpkg image already being written finishes the current file first, then stops.\n• Remaining queued jobs will not start.\n\nPartial/unfinished output and any temporary files are deleted. Jobs that finish, including the one running now if it completes, are kept.'):
            return
        uq_state['cancel'] = True
        uq_state['running'] = False
        try:
            btn_cancel.config(state='disabled', text='Cancelling…')
        except Exception:
            pass
        try:
            uq_status.set('Cancelling — stopping the current build…')
        except Exception:
            pass
        try:
            app._log('[UNIBUILD] Cancel requested by user.\n')
        except Exception:
            return
    uq_status_lbl = tk.Label(sum_card.body, textvariable=uq_status, font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left')
    uq_status_lbl.pack(fill='x', pady=(px(10), 0))
    autowrap(uq_status_lbl)

    # ── Right column: advanced performance (collapsible) ──
    apo_state = {'open': False}
    apo_exp = Expander(right, 'Advanced performance', caption='copy threads · compression', icon='advanced', bg=BG1)
    apo_exp.pack(fill='x', pady=(px(16), 0))
    apo_body = Card(apo_exp.body, padding=16)
    apo_body.pack(fill='x')

    def _apo_toggle(_e=None):
        apo_exp.toggle()
        apo_state['open'] = apo_exp.is_open

    def _apo_group_header(parent, text):
        hdr = tk.Frame(parent, bg=BG2)
        tk.Label(hdr, text=text.upper(), font=FONTS['overline'], bg=BG2, fg=COLORS['fg_4'], anchor='w').pack(fill='x')
        return hdr
    grp_copy = tk.Frame(apo_body, bg=BG2)
    grp_copy.pack(fill='x')
    _apo_group_header(grp_copy, 'Copy performance').pack(fill='x')
    cpu_row = tk.Frame(grp_copy, bg=BG2)
    cpu_row.pack(fill='x', pady=(px(8), 0))
    cpu_lcol = tk.Frame(cpu_row, bg=BG2)
    cpu_lcol.pack(side='left', fill='x', expand=True)
    tk.Label(cpu_lcol, text='Build copy threads', font=FONTS['body'], bg=BG2, fg=COLORS['fg_1'], anchor='w').pack(fill='x')
    n_cores = max(1, os.cpu_count() or 1)
    autowrap(tk.Label(cpu_lcol, text='Parallel file copies during the image build (robocopy /MT) — %d cores available. Does NOT affect .ffpfsc compression, which uses a separate memory-safe worker limit automatically.' % n_cores, font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left'))
    cpu_lcol.winfo_children()[-1].pack(fill='x', pady=(px(2), 0))
    _core_steps = [*(1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128)]
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
    cpu_box = Dropdown(cpu_row, textvariable=app._adv_threads_var, values=core_vals, width=6)
    cpu_box.pack(side='right', anchor='n', padx=(px(12), 0))

    def _on_cores_pick(_e=None):
        try:
            app._adv_clamp_threads()
            app._save_adv_params()
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            pass
    cpu_box.bind('<<ComboboxSelected>>', _on_cores_pick)
    grp_comp = tk.Frame(apo_body, bg=BG2)
    grp_comp.pack(fill='x', pady=(px(16), 0))
    _apo_group_header(grp_comp, 'Compression').pack(fill='x')
    comp_na = tk.Frame(grp_comp, bg=BG2)
    tk.Label(comp_na, text='Not applicable', font=FONTS['body'], bg=BG2, fg=COLORS['fg_2'], anchor='w').pack(fill='x')
    comp_na_desc = tk.Label(comp_na, text='Native exFAT images are not compressed.', font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left')
    comp_na_desc.pack(fill='x', pady=(px(2), 0))
    comp_ctl = tk.Frame(grp_comp, bg=BG2)
    be_row = tk.Frame(comp_ctl, bg=BG2)
    be_row.pack(fill='x', pady=(px(8), 0))
    tk.Label(be_row, text='Backend', font=FONTS['body'], bg=BG2, fg=COLORS['fg_1'], anchor='w').pack(fill='x')
    _BE_LAZY_LABEL = 'Lazy MkPFS (Default)'
    _BE_MKPFS_LABEL = 'Stock MkPFS — compatibility fallback'

    def _apo_save_backend():
        try:
            val = (pfs_backend.get() or 'lazy').strip().lower()
            if val not in ('lazy', 'mkpfs'):
                val = 'lazy'
            app._settings['pfs_backend'] = val
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            pass
        try:
            _refresh_apo_groups()
        except NameError:
            return
    XRadio(be_row, text=_BE_LAZY_LABEL, value='lazy', variable=pfs_backend, command=_apo_save_backend).pack(anchor='w', pady=(px(4), 0))
    _apo_exp_state = {'open': pfs_backend.get() == 'mkpfs'}
    _apo_exp_var = tk.BooleanVar(value=_apo_exp_state['open'])
    exp_wrap = tk.Frame(be_row, bg=BG2)
    exp_wrap.pack(fill='x', pady=(px(2), 0))
    mkpfs_radio_frame = tk.Frame(exp_wrap, bg=BG2)
    XRadio(mkpfs_radio_frame, text=_BE_MKPFS_LABEL, value='mkpfs', variable=pfs_backend, command=_apo_save_backend).pack(side='left')

    def _apo_toggle_exp():
        _apo_exp_state['open'] = bool(_apo_exp_var.get())
        if _apo_exp_state['open']:
            mkpfs_radio_frame.pack(anchor='w', padx=(px(24), 0), pady=(px(2), 0))
            return
        mkpfs_radio_frame.pack_forget()
    XCheck(exp_wrap, text='Show experimental compression backends', variable=_apo_exp_var, command=_apo_toggle_exp).pack(anchor='w')
    eng_row = tk.Frame(comp_ctl, bg=BG2)
    eng_row.pack(fill='x', pady=(px(12), 0))
    tk.Label(eng_row, text='Compression engine', font=FONTS['body'], bg=BG2, fg=COLORS['fg_1'], anchor='w').pack(fill='x')
    _apo_isa_var = tk.BooleanVar(value=pfs_zlib_backend.get() == 'isa-l')

    def _apo_save_engine():
        try:
            val = 'isa-l' if _apo_isa_var.get() else 'zlib'
            pfs_zlib_backend.set(val)
            app._settings['pfs_lazy_zlib_backend'] = val
            try:
                stored = int(app._settings.get('pfs_compression_level', 9))
            except Exception:
                stored = 9
            if val == 'isa-l':
                lv = 0 if stored <= 0 else 3 if stored >= 3 else 1
                app._settings['pfs_compression_level'] = max(0, min(3, lv))
            else:
                lv = stored if stored >= 1 else 1
                app._settings['pfs_compression_level'] = max(1, min(9, lv))
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            pass
        try:
            _apo_sync_engine_var()
        except NameError:
            pass
        try:
            _refresh_apo_groups()
        except NameError:
            return
    eng_choice = tk.Frame(eng_row, bg=BG2)
    eng_choice.pack(fill='x', pady=(px(4), 0))
    XRadio(eng_choice, text='zlib — safe (default)', value=False, variable=_apo_isa_var, command=_apo_save_engine).pack(anchor='w')
    XRadio(eng_choice, text='zlib-isa — experimental', value=True, variable=_apo_isa_var, command=_apo_save_engine).pack(anchor='w')
    eng_isa_note = tk.Label(eng_row, text='Experimental: uses zlib-isa acceleration for faster compression. PS5 mount compatibility is still under validation — use only for testing.', font=FONTS['caption'], bg=BG2, fg=COLORS['warn_hi'], anchor='w', justify='left')
    autowrap(eng_isa_note)
    lvl_row = tk.Frame(comp_ctl, bg=BG2)
    lvl_row.pack(fill='x', pady=(px(12), 0))
    lvl_lcol = tk.Frame(lvl_row, bg=BG2)
    lvl_lcol.pack(side='top', fill='x')
    tk.Label(lvl_lcol, text='Compression level', font=FONTS['body'], bg=BG2, fg=COLORS['fg_1'], anchor='w').pack(fill='x')
    lvl_desc = tk.Label(lvl_lcol, text='Higher = smaller output, more CPU. All levels mount fine in ShadowMount.', font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left')
    lvl_desc.pack(fill='x', pady=(px(2), px(6)))
    autowrap(lvl_desc)
    _CLVL_LABELS = {'9': '9 — Stable / safest (default)', '6': '6 — Balanced / faster', '3': '3 — Fast / larger output', '1': '1 — Fastest / largest'}
    try:
        _cur_lvl = int(app._settings.get('pfs_compression_level', 9))
    except Exception:
        _cur_lvl = 9
    _cur_lvl = max(1, min(9, _cur_lvl))
    app._pfs_clvl_var = tk.StringVar(value=_CLVL_LABELS.get(str(_cur_lvl), _CLVL_LABELS['9']))
    clvl_vals = [_CLVL_LABELS['9'], _CLVL_LABELS['6'], _CLVL_LABELS['3'], _CLVL_LABELS['1']]
    clvl_box = Dropdown(lvl_row, textvariable=app._pfs_clvl_var, values=clvl_vals, width=26)

    def _on_clvl_pick(_e=None):
        try:
            sel = app._pfs_clvl_var.get()
            lvl = int(sel.split()[0])  # leading digit
            app._settings['pfs_compression_level'] = max(1, min(9, lvl))
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            pass
    clvl_box.bind('<<ComboboxSelected>>', _on_clvl_pick)
    _ISA_LEVEL_TO_LABEL = {0: 'Fast', 1: 'Balanced', 3: 'Maximum'}
    _ISA_LABEL_TO_LEVEL = {'Fast': 0, 'Balanced': 1, 'Maximum': 3}

    def _isa_label_for(level):
        try:
            lv = int(level)
        except Exception:
            lv = 3
        if lv <= 0:
            return 'Fast'
        if lv >= 3:
            return 'Maximum'
        return 'Balanced'
    app._pfs_isa_lvl_var = tk.StringVar(value=_isa_label_for(_cur_lvl))
    isa_lvl_box = Dropdown(lvl_row, textvariable=app._pfs_isa_lvl_var, values=[*('Fast', 'Balanced', 'Maximum')], width=26)

    def _on_isa_lvl_pick(_e=None):
        try:
            sel = app._pfs_isa_lvl_var.get()
            lvl = _ISA_LABEL_TO_LEVEL.get(sel, 3)
            app._settings['pfs_compression_level'] = max(0, min(3, lvl))
            from exfat_builder import save_settings
            save_settings(app._settings)
        except Exception:
            return
    isa_lvl_box.bind('<<ComboboxSelected>>', _on_isa_lvl_pick)

    def _apo_sync_engine_var():
        try:
            stored = int(app._settings.get('pfs_compression_level', 9))
        except Exception:
            stored = 9
        try:
            app._pfs_isa_lvl_var.set(_isa_label_for(stored))
        except Exception:
            pass
        try:
            zlv = max(1, min(9, stored)) if stored >= 1 else 9
            app._pfs_clvl_var.set(_CLVL_LABELS.get(str(zlv), _CLVL_LABELS['9']))
        except Exception:
            return

    def _refresh_apo_groups():
        is_pfs = out_type.get() == 'pfs'
        if is_pfs:
            try:
                comp_na.pack_forget()
            except Exception:
                pass
            comp_ctl.pack(fill='x')
            _is_lazy = (pfs_backend.get() or 'lazy').strip().lower() == 'lazy'
            if _is_lazy:
                eng_row.pack(fill='x', pady=(px(12), 0), before=lvl_row)
                if _apo_isa_var.get():
                    eng_isa_note.pack(fill='x', pady=(px(4), 0))
                else:
                    try:
                        eng_isa_note.pack_forget()
                    except Exception:
                        pass
            else:
                try:
                    eng_row.pack_forget()
                    eng_isa_note.pack_forget()
                except Exception:
                    pass
            _use_isa = _is_lazy and bool(_apo_isa_var.get())
            if _use_isa:
                try:
                    clvl_box.pack_forget()
                except Exception:
                    pass
                isa_lvl_box.pack(side='top', fill='x')
                return
            try:
                isa_lvl_box.pack_forget()
            except Exception:
                pass
            clvl_box.pack(side='top', fill='x')
            return
        try:
            comp_ctl.pack_forget()
        except Exception:
            pass
        comp_na.pack(fill='x', pady=(px(8), 0))

    # ── Right column: performance tips (collapsible) ──
    tips_state = {'open': False}
    tips_exp = Expander(right, 'Performance tips', caption='help · not settings', icon='lightbulb', bg=BG1)
    tips_exp.pack(fill='x', pady=(px(8), 0))
    tips_body = Card(tips_exp.body, padding=16)
    tips_body.pack(fill='x')
    tip_lines = [*(('Use separate physical drives.', 'Put the game dump, the Output folder, and the Temp folder on three different drives. PFS compression reads the whole image and writes the result at the same time — on one disk that contention drops you to a few MB/s with the CPU sitting idle.'), ('Point Temp at your fastest drive (ideally NVMe SSD).', 'During compression the .ffpfsc is written to the Temp drive so the read and write hit different disks, then moved to the Output folder. A fast Temp drive is the single biggest speed-up for large games.'), ('Give Temp enough free space.', 'Because the compressed image is staged on the Temp drive, it needs room for it (budget about 2x the image size, transiently). If Temp is too small the build automatically falls back to writing beside the source — safe, but slower.'), ('Build copy threads affect the copy, not the squeeze.', 'The copy-threads selector controls parallel file copies during the build (robocopy /MT). PFS compression uses a separate, memory-safe worker count that is set automatically — it is not controlled by that setting.'))]
    for _ti, (_head, _desc) in enumerate(tip_lines):
        row = tk.Frame(tips_body, bg=BG2)
        row.pack(fill='x', pady=(0 if _ti == 0 else px(12), 0))
        tk.Label(row, text=_head, font=FONTS['body_strong'], bg=BG2, fg=COLORS['fg_0'], anchor='w', justify='left').pack(fill='x')
        autowrap(tk.Label(row, text=_desc, font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w', justify='left'))
        row.winfo_children()[-1].pack(fill='x', pady=(px(2), 0))

    def _toggle_tips(_e=None):
        tips_exp.toggle()
        tips_state['open'] = tips_exp.is_open
    _apo_toggle_exp()
    _refresh_apo_groups()

    # ── Progress card (full width, shown while building) ──
    status_card = Card(inner, title='Build progress', padding=16)
    sc_in = status_card.body
    prog_frame = tk.Frame(sc_in, bg=BG2)
    prog_top = tk.Frame(prog_frame, bg=BG2)
    prog_top.pack(fill='x')
    tk.Label(prog_top, textvariable=uq_step_var, font=FONTS['body_strong'], bg=BG2, fg=COLORS['fg_0'], anchor='w').pack(side='left')
    tk.Label(prog_top, textvariable=uq_pct_var, font=FONTS['body'], bg=BG2, fg=COLORS['fg_2'], anchor='e').pack(side='right')
    track = RoundedBar(prog_frame, height=6, surface=BG2)
    track.pack(fill='x', pady=(px(6), px(4)))
    fill = track.fill
    tk.Label(prog_frame, textvariable=uq_eta_var, font=FONTS['caption'], bg=BG2, fg=COLORS['fg_3'], anchor='w').pack(fill='x')
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
    def _gen_name(*_):
        folder = src_var.get().strip()
        if not folder:
            return
        ext = _TYPE_EXT[out_type.get()]
        tid = (detected.get('title_id') or '').strip()
        title = (detected.get('title') or '').strip()
        ver = (detected.get('version') or '').strip()
        fw = (detected.get('firmware') or '').strip()
        preset = name_preset.get()
        if preset == 'custom':
            _ov = (name_fw_override.get() or 'auto').strip()
            eff_fw = _ov if _ov and _ov != 'auto' else fw
            fw_tok = 'FW' + eff_fw if eff_fw else ''
            try:
                type_tok = _TYPE_LABEL[out_type.get()]
            except Exception:
                type_tok = out_type.get()
            date_tok = time.strftime('%Y-%m-%d')
            tmpl = name_template.get()
            _subs = {'%TITLE%': title, '%PPSA%': tid, '%VERSION%': ver, '%FIRMWARE%': fw_tok, '%TYPE%': type_tok, '%DATE%': date_tok}
            base = tmpl
            for _k, _v in _subs.items():
                base = re.sub(re.escape(_k), _v, base, flags=re.IGNORECASE)
            for _ in range(4):
                _new = re.sub('\\(\\s*\\)|\\[\\s*\\]|\\{\\s*\\}', '', base)
                if _new == base:
                    break
                base = _new
            base = re.sub('\\s{2,}', ' ', base).strip()
            base = re.sub('\\s+([)\\]}])', '\\1', base)
            base = re.sub('([(\\[{])\\s+', '\\1', base)
            base = base.strip(' -_–—')
        elif preset == 'ppsa':
            base = tid
        elif preset == 'ppsa_title':
            base = (tid + ' ' + title).strip() if title else tid
        elif preset == 'ppsa_title_ver_fw':
            nm = (tid + ' ' + title).strip() if title else tid
            if ver:
                nm = nm + ' (' + ver + ')'
            _ov = (name_fw_override.get() or 'auto').strip()
            eff_fw = _ov if _ov and _ov != 'auto' else fw
            base = nm + ' (FW' + eff_fw + ')' if eff_fw else nm
        else:
            nm = (tid + ' ' + title).strip() if title else tid
            base = nm + ' (' + ver + ')' if ver else nm
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
            ok = bool(src and (os.path.isfile(src) and os.path.splitext(src)[1].lower() in ('.exfat', '.ffpkg')))
            _hint = '' if ok else 'Pick an .exfat or .ffpkg image to continue.' if not src else 'That file isn\\u2019t an .exfat or .ffpkg image.'
        else:
            ok = bool(src and (os.path.isdir(src) and (detected.get('title_id') or detected.get('title'))))
            if ok:
                _hint = ''
            elif not src:
                _hint = 'Pick a game dump folder to continue.'
            elif not os.path.isdir(src):
                _hint = 'That folder couldn\\u2019t be found \\u2014 check the path.'
            else:
                _hint = 'Couldn\\u2019t detect a PS5 game here \\u2014 pick the folder that contains eboot.bin.'
        if ok:
            ready_badge.pack(side='left')
            try:
                ready_hint.pack_forget()
                ready_hint.config(text='')
                return
            except Exception:
                return
        ready_badge.pack_forget()
        try:
            ready_hint.config(text=_hint)
            ready_hint.pack(side='left')
        except Exception:
            return
    def _update_space(*_):
        _space_gen['n'] += 1
        my_gen = _space_gen['n']
        folder = src_var.get().strip()
        outdir = outdir_var.get().strip()
        t = out_type.get()
        _img_mode = t == 'pfs' and pfs_src_mode.get() == 'image'
        tmp = tempdir_var.get().strip() or str(app._settings.get('temp_dir') or '').strip()
        _src_ok = os.path.isfile(folder) if _img_mode else os.path.isdir(folder)
        if not folder or not _src_ok:
            for v in hero_size_var, hero_free_var, stat_estsize_var, stat_needed_var, stat_space_var:
                v.set('—')
            disk_cell.grid_remove()
            space_state.update({'ok': True, 'out_need': 0, 'tmp_need': 0, 'detail': ''})
            return
        def _w():
            import shutil as _sh
            try:
                size = os.path.getsize(folder) if _img_mode else app._get_folder_size(folder, stale_check=(lambda: my_gen != _space_gen['n']))
            except Exception:
                size = 0
            def _drive(p):
                try:
                    return os.path.splitdrive(os.path.abspath(p))[0].upper()
                except Exception:
                    return ''
            out_drive = _drive(outdir) if outdir else ''
            tmp_drive = _drive(tmp) if tmp else ''
            src_drive = _drive(folder)
            same_drive = bool(out_drive and (tmp_drive and out_drive == tmp_drive))
            if t == 'pfs':
                if _img_mode:
                    out_need = int(size * (1.05 if src_drive == out_drive else 2.05))
                else:
                    out_need = int(size * 2.05)
                if tmp:
                    tmp_need = int(size * (1.0 if same_drive else 2.2))
                else:
                    tmp_need = 0
            else:
                out_need = int(size * 1.12)
                tmp_need = 0
            out_free = tmp_free = None
            try:
                if outdir and os.path.isdir(outdir):
                    out_free = _sh.disk_usage(outdir).free
            except Exception:
                out_free = None
            try:
                if tmp and os.path.isdir(tmp):
                    tmp_free = _sh.disk_usage(tmp).free
            except Exception:
                tmp_free = None
            if same_drive:
                combined = out_need + tmp_need
                out_short = out_free is not None and out_free < combined
                tmp_short = out_short
            else:
                out_short = out_free is not None and out_free < out_need
                tmp_short = tmp_need > 0 and (tmp_free is not None and tmp_free < tmp_need)
            detail = ''
            if same_drive and tmp_need > 0:
                detail = f"Output + Temp share {out_drive or 'one drive'!s} — needs ~{_fmt_b(out_need + tmp_need)!s} combined, {_fmt_b(out_free) if out_free is not None else 'unknown'!s} free"
            elif tmp_need > 0:
                detail = f"Temp {tmp_drive or tmp!s} — needs ~{_fmt_b(tmp_need)!s}, {_fmt_b(tmp_free) if tmp_free is not None else 'unknown'!s} free"
            if my_gen != _space_gen['n']:
                return
            space_state.update({'ok': not (out_short or tmp_short), 'out_need': out_need, 'tmp_need': tmp_need, 'out_free': out_free, 'tmp_free': tmp_free, 'same_drive': same_drive, 'out_drive': out_drive, 'tmp_drive': tmp_drive, 'detail': detail})
            def _set():
                if my_gen != _space_gen['n']:
                    return
                hero_size_var.set(_fmt_b(size))
                if t == 'pfs':
                    stat_estsize_var.set(f"~{_fmt_b(size * 0.55)!s}–{_fmt_b(size * 0.95)!s}")
                else:
                    stat_estsize_var.set('~%s' % _fmt_b(size))
                stat_needed_var.set('~%s' % _fmt_b(out_need))
                needed_val.config(fg=COLORS['danger_hi'] if out_short else COLORS['fg_0'])
                if out_free is not None:
                    tag = '  ⚠' if out_short else ''
                    hero_free_var.set(_fmt_b(out_free) + tag)
                    stat_space_var.set(_fmt_b(out_free) + ' free' + tag)
                    space_val.config(fg=COLORS['danger_hi'] if out_short else COLORS['fg_0'])
                    free_val.config(fg=COLORS['danger_hi'] if out_short else COLORS['fg_0'])
                else:
                    hero_free_var.set('—')
                    stat_space_var.set('—')
                    space_val.config(fg=COLORS['fg_0'])
                if detail:
                    short_here = tmp_short or same_drive and out_short
                    if short_here:
                        stat_disk_var.set('⚠  NOT ENOUGH SPACE — ' + detail)
                    else:
                        stat_disk_var.set(detail)
                    disk_val.config(fg=COLORS['danger_hi'] if short_here else COLORS['fg_0'])
                    disk_cell.grid()
                    return
                disk_cell.grid_remove()
            parent.after(0, _set)
        threading.Thread(target=_w, daemon=True).start()
    def _on_source_changed(*_):
        src = src_var.get().strip()
        if out_type.get() == 'pfs' and pfs_src_mode.get() == 'image':
            if not src or not os.path.isfile(src):
                return
            if not _out_user_choice['set'] and not outdir_var.get().strip():
                _set_outdir_programmatic(os.path.dirname(src) or '.')
            import re as _re
            stem = os.path.splitext(os.path.basename(src))[0]
            m = _re.search('((?:PPSA|CUSA|PPLH)\\d{5})', stem, _re.IGNORECASE)
            tid = m.group(1).upper() if m else ''
            mv = _re.search('(\\d{2}\\.\\d{3}\\.\\d{3})', stem)
            ver = mv.group(1) if mv else ''
            ttl = stem
            if tid:
                ttl = _re.sub('(?i)' + _re.escape(tid), '', ttl)
            if ver:
                ttl = _re.sub('\\(\\s*' + _re.escape(ver) + '\\s*\\)', '', ttl)
                ttl = ttl.replace(ver, '')
            ttl = _re.sub('\\(\\s*\\)', '', ttl)
            ttl = _re.sub('\\s{2,}', ' ', ttl).strip(' -_()')
            detected['title'] = ttl
            detected['title_id'] = tid
            detected['version'] = ver
            detected['folder'] = ''
            detected['firmware'] = ''
            det_title_var.set(ttl or (tid or stem))
            det_id_var.set(tid or '—')
            hero_ver_var.set(ver or '—')
            cover_ref['img'] = None
            try:
                cover_lbl.config(image='', text='🗜')
            except Exception:
                pass
            _gen_name()
            _refresh_ready()
            _update_space()
            return
        folder = src
        if not folder or not os.path.isdir(folder):
            return
        if not _out_user_choice['set'] and not outdir_var.get().strip():
            _par = os.path.dirname(folder.rstrip('/\\'))
            _set_outdir_programmatic(_par or folder)
        det_title_var.set('Reading game info…')
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
                m = _re.search('((?:PPSA|CUSA|PPLH)\\d{5})', os.path.basename(folder.rstrip('/\\')), _re.IGNORECASE)
                if m:
                    title_id = m.group(1).upper()
            detected['title'] = title
            detected['title_id'] = title_id
            detected['version'] = version
            detected['folder'] = folder
            try:
                _fw_major, _fw_minor = app._abp_read_required_fw(folder)
                detected['firmware'] = str(_fw_major) if _fw_major is not None else ''
            except Exception:
                detected['firmware'] = ''
            got_cover = {'ok': False}
            try:
                cp = app._load_cover_art(folder)
                if cp:
                    from exfat_builder import _load_cover_image
                    pil = _load_cover_image(cp, target=px(112))
                    if pil is not None:
                        def _setimg(p=pil):
                            try:
                                from PIL import ImageTk as _IT
                                cover_ref['img'] = _IT.PhotoImage(p)
                                cover_lbl.config(image=cover_ref['img'], text='', width=p.width, height=p.height)
                            except Exception:
                                return
                        got_cover['ok'] = True
                        parent.after(0, _setimg)
            except Exception:
                pass
            def _done():
                if not got_cover['ok']:
                    cover_ref['img'] = None
                    cover_lbl.config(image='', text='🎮')
                disp = title or (title_id or os.path.basename(folder.rstrip('/\\')))
                det_title_var.set(disp)
                det_id_var.set(title_id or '—')
                hero_ver_var.set(version or '—')
                _gen_name()
                _refresh_ready()
            parent.after(0, _done)
            _update_space_debounced()
        threading.Thread(target=_w, daemon=True).start()
    src_var.trace_add('write', _on_source_changed)
    _space_pend = {'id': None}
    def _update_space_debounced(*_a):
        if _space_pend['id'] is not None:
            try:
                app.after_cancel(_space_pend['id'])
            except Exception:
                pass
            _space_pend['id'] = None
        def _fire():
            _space_pend['id'] = None
            _update_space()
        try:
            _space_pend['id'] = app.after(300, _fire)
        except Exception:
            _update_space()
            return
    def _on_outdir_write(*_):
        if not _out_programmatic['on']:
            _out_user_choice['set'] = True
        _update_space_debounced()
    outdir_var.trace_add('write', _on_outdir_write)
    tempdir_var.trace_add('write', _update_space_debounced)
    def _uq_set_progress(pct, step, eta):
        # Thread-safety: this mutates Tk widgets. If a worker thread reaches
        # it (e.g. via app._unified_progress), bounce onto the Tk thread.
        import threading as _thr
        if _thr.get_ident() != getattr(app, '_tk_thread_id', None):
            try:
                app.after(0, _uq_set_progress, pct, step, eta)
            except Exception:
                pass
            return
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
            prog_frame.pack(fill='x', pady=(0, px(14)), before=phase_row)
        else:
            prog_frame.pack_forget()
            _uq_set_progress(0, '', '')
    app._unified_progress = _uq_set_progress
    _pulse = {'on': False, 'pos': 0.0, 'dir': 1, 'after_id': None}
    def _pulse_step():
        if not _pulse['on']:
            return
        # Sweep a fixed-width band left<->right across the track.
        band = 0.22
        step = 0.035
        _pulse['pos'] += step * _pulse['dir']
        if _pulse['pos'] <= 0.0:
            _pulse['pos'] = 0.0
            _pulse['dir'] = 1
        elif _pulse['pos'] + band >= 1.0:
            _pulse['pos'] = 1.0 - band
            _pulse['dir'] = -1
        try:
            fill.place_configure(relx=_pulse['pos'], relwidth=band)
        except Exception:
            pass
        try:
            _pulse['after_id'] = app.after(60, _pulse_step)
        except Exception:
            pass
    def _pulse_start():
        if _pulse['on']:
            return
        _pulse['on'] = True
        _pulse['pos'] = 0.0
        _pulse['dir'] = 1
        try:
            uq_pct_var.set('')   # no fake percentage during indeterminate work
        except Exception:
            pass
        _pulse_step()
    def _pulse_stop():
        _pulse['on'] = False
        if _pulse['after_id']:
            try:
                app.after_cancel(_pulse['after_id'])
            except Exception:
                pass
            _pulse['after_id'] = None
        try:
            fill.place_configure(relx=0.0)   # back to left-anchored fill
        except Exception:
            pass
    _hb = {'on': False, 'after_id': None, 't0': 0.0, 'phase': 5, 'io0': None, 'iot': 0.0, 'detail_i': 0, 'src_size': 0}
    _HB_DETAILS = [*('Opening source image...', 'Reading image layout...', 'Preparing compression workers...', 'Waiting for first compression progress...')]
    def _hb_sample_disk():
        """Return (read_str, write_str) since the last sample, or (—, —)."""
        try:
            import psutil as _ps
            now = time.time()
            cur = _ps.disk_io_counters()
            prev = _hb['io0']
            _hb['io0'] = cur
            dt = now - _hb['iot']
            _hb['iot'] = now
            if prev is None or dt <= 0:
                return ('\u2014', '\u2014')
            rd = max(0, (cur.read_bytes - prev.read_bytes)) / dt
            wr = max(0, (cur.write_bytes - prev.write_bytes)) / dt
            def _mb(x):
                return ('%.0f MB/s' % (x / 1048576.0)) if x >= 1048576 \
                    else ('%.0f KB/s' % (x / 1024.0))
            return (_mb(rd), _mb(wr))
        except Exception:
            return ('\u2014', '\u2014')
    def _hb_tick():
        if not _hb['on']:
            return
        if getattr(app, '_pfs_compression_started', False):
            _hb_stop()
            return
        el = max(0, int(time.time() - _hb['t0']))
        mm, ss = el // 60, el % 60
        if el >= 180:
            detail = 'Still preparing. If this continues unusually long, check disk activity and the output log.'
        elif el >= 60:
            detail = 'Still working after %dm %02ds. Large images can take several minutes before compression progress appears.' % (mm, ss)
        else:
            detail = _HB_DETAILS[_hb['detail_i'] % len(_HB_DETAILS)]
            _hb['detail_i'] += 1
        elapsed_txt = '%dm %02ds elapsed' % (mm, ss)
        rd, wr = _hb_sample_disk()
        try:
            uq_step_var.set('Starting Compression')
            uq_eta_var.set(f"{detail!s}   ·   {elapsed_txt!s}")
            uq_pct_var.set('')
            metric_vars['current_file'].set('Disk Read: %s' % rd)
            metric_vars['speed'].set('Disk Write: %s' % wr)
        except Exception:
            pass
        try:
            _hb['after_id'] = app.after(2500, _hb_tick)
        except Exception:
            return
    def _hb_start(phase=5, src_size=0):
        if _hb['on']:
            return
        _hb['on'] = True
        _hb['t0'] = time.time()
        _hb['phase'] = phase
        _hb['detail_i'] = 0
        _hb['io0'] = None
        _hb['iot'] = time.time()
        _hb['src_size'] = src_size or 0
        app._pfs_compression_started = False
        try:
            metric_vars['progress'].set('—')
            metric_vars['files'].set('—')
        except Exception:
            pass
        try:
            app._unified_telemetry({'phase': phase})
        except Exception:
            pass
        _pulse_start()
        _hb_tick()
    def _hb_stop():
        if not _hb['on'] and not _pulse['on']:
            return
        _hb['on'] = False
        if _hb['after_id']:
            try:
                app.after_cancel(_hb['after_id'])
            except Exception:
                pass
            _hb['after_id'] = None
        _pulse_stop()
        # Clear the temporary disk-activity readouts.
        try:
            metric_vars['current_file'].set('\u2014')
        except Exception:
            pass
    app._uq_heartbeat_start = _hb_start
    app._uq_heartbeat_stop = _hb_stop
    app._uq_pulse_start = _pulse_start
    app._uq_pulse_stop = _pulse_stop
    PHASES_EXFAT = [*('Scan Source', 'Create Image', 'Format', 'Copy Files', 'Finalize')]
    PHASES_PFS = [*('Scan Source', 'Create exFAT', 'Format', 'Copy Files', 'Finalize', 'Compress')]
    PHASES = PHASES_PFS
    phase_row = tk.Frame(sc_in, bg=COLORS['bg_2'])
    phase_row.pack(fill='x', pady=(0, px(14)))
    phase_lbls = []
    def _build_phase_row(phases):
        for _w in phase_row.winfo_children():
            _w.destroy()
        phase_lbls.clear()
        for i, name in enumerate(phases):
            cell = tk.Frame(phase_row, bg=COLORS['bg_2'])
            cell.pack(side='left', expand=True, fill='x')
            icon = StepIcon(cell, '⏳', size=16, bg=COLORS['bg_2'])
            icon.pack(side='left')
            txt = tk.Label(cell, text=name, font=FONTS['caption'],
                           bg=COLORS['bg_2'], fg=COLORS['fg_4'])
            txt.pack(side='left', padx=(px(6), 0))
            phase_lbls.append((icon, txt))
    _build_phase_row(PHASES)
    def _set_phase_mode(is_pfs):
        """5-phase (exFAT/ffpkg) or 6-phase (FFPFSC) tracker. Called at build
        start from the selected output format."""
        _build_phase_row(PHASES_PFS if is_pfs else PHASES_EXFAT)
    app._uq_set_phase_mode = _set_phase_mode
    Divider(sc_in, color=COLORS['border_1']).pack(fill='x', pady=(0, px(12)))
    grid = tk.Frame(sc_in, bg=COLORS['bg_2'])
    grid.pack(fill='x')
    metric_vars = {}
    def _metric(parent, key, label, col, rowi):
        cell = tk.Frame(parent, bg=COLORS['bg_2'])
        cell.grid(row=rowi, column=col, sticky='ew', padx=(0, px(24)), pady=px(3))
        tk.Label(cell, text=label, font=FONTS['caption'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_3'],
                 width=14, anchor='w').pack(side='left')
        v = tk.StringVar(value='—')
        tk.Label(cell, textvariable=v, font=FONTS['body'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_1'], anchor='w').pack(
                     side='left', fill='x', expand=True)
        metric_vars[key] = v
    _rows = [*(('source', 'Source', 'output', 'Output'), ('current_file', 'Current file', 'progress', 'Progress'), ('files', 'Files', 'speed', 'Speed'), ('eta', 'Time left', 'pfs', 'Compression'), ('temp', 'Temp used', 'temp_free', 'Temp free'), ('cpu', 'CPU', 'ram', 'Memory'))]
    for ri, (k1, l1, k2, l2) in enumerate(_rows):
        _metric(grid, k1, l1, 0, ri)
        _metric(grid, k2, l2, 1, ri)
    grid.grid_columnconfigure(0, weight=1, uniform='m')
    grid.grid_columnconfigure(1, weight=1, uniform='m')
    def _fmt_gb(b):
        try:
            b = float(b)
        except Exception:
            return '\u2014'
        if b >= 1024 ** 3:
            return '%.2f GB' % (b / 1024 ** 3)
        if b >= 1024 ** 2:
            return '%.0f MB' % (b / 1024 ** 2)
        return '%d B' % int(b)
    def _telemetry(t):
        # Bounce to Tk thread.
        import threading as _thr
        if _thr.get_ident() != getattr(app, '_tk_thread_id', None):
            try:
                app.after(0, _telemetry, t)
            except Exception:
                pass
            return
        try:
            phase = t.get('phase')
            if phase is not None:
                for i, (icon, txt) in enumerate(phase_lbls):
                    if i < phase:
                        icon.config(text='\u2714', fg=COLORS['fg_0'])
                        txt.config(fg=COLORS['fg_3'])
                    elif i == phase:
                        icon.config(text='\u25b6', fg=COLORS['fg_0'])
                        txt.config(fg=COLORS['fg_0'])
                    else:
                        icon.config(text='\u23f3', fg=COLORS['fg_5'])
                        txt.config(fg=COLORS['fg_5'])
            if 'source' in t:
                metric_vars['source'].set(t['source'] or '\u2014')
            if 'output' in t:
                metric_vars['output'].set(t['output'] or '\u2014')
            cf = t.get('current_file')
            if cf is not None:
                metric_vars['current_file'].set(cf or '\u2014')
            bd, bt = t.get('bytes_done'), t.get('bytes_total')
            if bd is not None and bt:
                metric_vars['progress'].set(
                    '%s / %s' % (_fmt_gb(bd), _fmt_gb(bt)))
                # v3.6.2: during Copy, make the TOP progress line match the
                # grid — byte counts + byte-throughput ETA, never robocopy's
                # per-file percentage. This is the single source of truth.
                if t.get('phase') == 3:
                    try:
                        frac = (float(bd) / float(bt)) if bt else 0.0
                        frac = max(0.0, min(1.0, frac))
                        uq_step_var.set('Copying game files \u2014 %s / %s'
                                        % (_fmt_gb(bd), _fmt_gb(bt)))
                        uq_pct_var.set('%d%%' % int(frac * 100))
                        fill.place_configure(relx=0.0, relwidth=frac)
                        _eta = t.get('eta')
                        uq_eta_var.set((_eta or '').replace('ETA: ', '')
                                       .replace('ETA ', ''))
                    except Exception:
                        pass
            elif t.get('pfs_pct') is not None:
                # PFS compression reports a percent of input processed, not a
                # byte count — surface it as the PROGRESS readout so the card
                # isn't blank while mkpfs is clearly running.
                # v3.6.2: real progress arrived — kill the startup heartbeat
                # + indeterminate pulse and drive the real bar/percent.
                try:
                    app._pfs_compression_started = True
                except Exception:
                    pass
                try:
                    app._uq_heartbeat_stop()
                except Exception:
                    pass
                try:
                    pct_val = int(t['pfs_pct'])
                    metric_vars['progress'].set('%d%%' % pct_val)
                    frac = max(0.0, min(1.0, pct_val / 100.0))
                    fill.place_configure(relx=0.0, relwidth=frac)
                    uq_pct_var.set('%d%%' % pct_val)
                    uq_step_var.set('Compressing PFS')
                    # v3.6.3: state the applied mkpfs worker count explicitly
                    # so the card never reads as if the Build copy threads
                    # setting was used here (it isn't — see _pfs_pack_tuning).
                    if t.get('pfs_workers'):
                        metric_vars['current_file'].set(
                            'Compression workers: %d (memory-safe limit)'
                            % t['pfs_workers'])
                except Exception:
                    pass
            fd, ft = t.get('files_done'), t.get('files_total')
            if fd and ft:
                # Final guard: copied can never exceed total (defends the
                # display against any upstream over-count, e.g. 532/103).
                if fd > ft:
                    fd = ft
                metric_vars['files'].set('{:,} / {:,}'.format(fd, ft))
            elif fd:
                metric_vars['files'].set('{:,}'.format(fd))
            sp = t.get('speed_mbs')
            if sp:
                metric_vars['speed'].set('%.0f MB/s' % sp)
            if t.get('pfs_throughput'):
                metric_vars['speed'].set(t['pfs_throughput'])
            # v3.6.2 hotfix: literal speed override (e.g. '—' during the
            # post-copy flush, when the last robocopy MB/s is stale).
            if t.get('speed_str'):
                metric_vars['speed'].set(t['speed_str'])
            if 'eta' in t and t['eta']:
                metric_vars['eta'].set(t['eta'].replace('ETA: ', '').replace(
                    'ETA ', ''))
            # v3.6.2: finalize-phase label driver. During the post-copy
            # bridge / flush / dismount (phase 4, no byte payload), surface
            # phase_label as the main progress line and pin the bar near the
            # end, so the card leaves "Copying game files / Almost done" the
            # instant the copy ends instead of sitting on the frozen frame.
            if (phase == 4 and t.get('phase_label')
                    and t.get('bytes_total') is None
                    and t.get('pfs_pct') is None):
                try:
                    uq_step_var.set(t['phase_label'])
                    uq_pct_var.set('99%')
                    fill.place_configure(relx=0.0, relwidth=0.99)
                    uq_eta_var.set('')
                except Exception:
                    pass
            # Compression cell: ratio% · L9 · 12w (PFS phase = index 5)
            if phase in (4, 5):
                bits = []
                if t.get('pfs_ratio') is not None:
                    bits.append('%d%%' % t['pfs_ratio'])
                if t.get('pfs_level'):
                    bits.append('L%d' % t['pfs_level'])
                if t.get('pfs_workers'):
                    bits.append('%d workers' % t['pfs_workers'])
                if bits:
                    metric_vars['pfs'].set(' \u00b7 '.join(bits))
            if 'temp_used' in t:
                tu = _fmt_gb(t['temp_used'])
                if t.get('temp_peak'):
                    tu += ' (peak %s)' % _fmt_gb(t['temp_peak'])
                metric_vars['temp'].set(tu)
            if 'temp_free' in t:
                metric_vars['temp_free'].set(_fmt_gb(t['temp_free']))
            if 'cpu' in t:
                metric_vars['cpu'].set('%.0f%%' % t['cpu'])
            if 'ram_used' in t and t.get('ram_total'):
                metric_vars['ram'].set('%s / %s' % (
                    _fmt_gb(t['ram_used']), _fmt_gb(t['ram_total'])))
        except Exception:
            pass
    app._unified_telemetry = _telemetry
    def _status_card_show(show):
        if show:
            status_card.pack(fill='x', padx=px(24), pady=(0, px(16)), before=cols)
        else:
            status_card.pack_forget()
            # v3.6.2: ensure any running heartbeat/pulse is stopped.
            try:
                _hb_stop()
            except Exception:
                pass
            # reset metrics + phases for next run
            for v in metric_vars.values():
                v.set('\u2014')
            for icon, txt in phase_lbls:
                icon.config(text='\u23f3', fg=COLORS['fg_5'])
                txt.config(fg=COLORS['fg_5'])
    app._unified_status_card_show = _status_card_show
    qcard = Card(inner, title='Build queue')
    uq_count_lbl = tk.Label(qcard.head_actions, textvariable=uq_count, font=FONTS['caption'], bg=COLORS['bg_2'], fg=COLORS['fg_3'])
    uq_count_lbl.pack(side='left', padx=(0, px(12)))
    Button(qcard.head_actions, text='Remove selected', icon='minus', size='sm', command=(lambda: _uq_remove_selected()), surface=COLORS['bg_2']).pack(side='left')
    Button(qcard.head_actions, text='Clear', icon='trash', variant='subtle', size='sm', command=(lambda: _uq_clear()), surface=COLORS['bg_2']).pack(side='left', padx=(px(4), 0))
    uq_list = tk.Frame(qcard.body, bg=COLORS['bg_2'])
    uq_list.pack(fill='x')
    _BADGE_TONE = {'queued': 'neutral', 'running': 'info', 'done': 'success',
                   'failed': 'danger', 'skipped': 'neutral', 'warn': 'warn'}
    _BADGE_WORD = {'queued': 'Queued', 'running': 'Building', 'done': 'Complete',
                   'failed': 'Failed', 'skipped': 'Skipped', 'warn': 'Check'}
    _BADGE_ICON = {'queued': 'queue', 'running': 'loader', 'done': 'check',
                   'failed': 'error', 'skipped': 'skip', 'warn': 'warning'}
    def _uq_render():
        import threading as _thr
        if _thr.get_ident() != getattr(app, '_tk_thread_id', None):
            try:
                app.after(0, _uq_render)
            except Exception:
                pass
            return
        for w in uq_list.winfo_children():
            w.destroy()
        try:
            _refresh_build_btn_label()
        except Exception:
            pass
        if queue:
            stat_jobs_var.set('%d job%s' % (len(queue), '' if len(queue) == 1 else 's'))
            jobs_val.config(fg=COLORS['fg_0'])
        else:
            stat_jobs_var.set('No queued jobs')
            jobs_val.config(fg=COLORS['fg_3'])
        if not queue:
            qcard.pack_forget()
            uq_count.set('')
            uq_state['sel'].clear()
            return
        qcard.pack(fill='x', padx=px(24), pady=(0, px(16)))
        done = sum(1 for j in queue if j['status'] == 'done')
        failed = sum(1 for j in queue if j['status'] == 'failed')
        lab = '%d job%s  ·  %d done' % (len(queue), '' if len(queue) == 1 else 's', done)
        if failed:
            lab += '  ·  %d failed' % failed
        warned = sum(1 for j in queue if j['status'] == 'warn')
        if warned:
            lab += '  ·  %d to check' % warned
        uq_count.set(lab)
        present = {id(j) for j in queue}
        for k in [k for k in uq_state['sel'] if k not in present]:
            del uq_state['sel'][k]
        running = uq_state['running']
        n = len(queue)
        RB = COLORS['bg_2']
        for idx, job in enumerate(queue):
            editable = job['status'] == 'queued'
            if idx:
                Divider(uq_list, color=COLORS['border_1']).pack(fill='x')
            row = tk.Frame(uq_list, bg=RB)
            row.pack(fill='x')
            inner_r = tk.Frame(row, bg=RB)
            inner_r.pack(fill='x', pady=px(6))
            if editable:
                var = uq_state['sel'].get(id(job))
                if var is None:
                    var = tk.BooleanVar(value=False)
                    uq_state['sel'][id(job)] = var
                XCheck(inner_r, variable=var, surface=RB).pack(side='left', padx=(0, px(8)))
            else:
                tk.Frame(inner_r, bg=RB, width=px(32), height=1).pack(side='left')
            Badge(inner_r, _TYPE_LABEL[job['type']], 'outline', surface=RB).pack(side='left', padx=(0, px(12)))
            label = job['name']
            sub = ''
            if job['type'] == 'pfs':
                sub = 'from image' if job.get('from_image') else 'via %s' % _TYPE_LABEL[job.get('via') or 'exfat']
            tk.Label(inner_r, text=label, font=FONTS['body'], bg=RB, fg=COLORS['fg_1'], anchor='w').pack(side='left')
            if sub:
                tk.Label(inner_r, text=sub, font=FONTS['caption'], bg=RB, fg=COLORS['fg_3'], anchor='w').pack(side='left', padx=(px(8), 0))
            tk.Frame(inner_r, bg=RB).pack(side='left', fill='x', expand=True)
            if editable:
                IconButton(inner_r, 'trash', (lambda j=job: _uq_remove_job(j)), tooltip='Remove from queue', size='sm', surface=RB).pack(side='right')
                if idx < n - 1:
                    IconButton(inner_r, 'arrow_down', (lambda i=idx: _uq_move(i, 1)), tooltip='Move down', size='sm', surface=RB).pack(side='right')
                if idx > 0:
                    IconButton(inner_r, 'arrow_up', (lambda i=idx: _uq_move(i, -1)), tooltip='Move up', size='sm', surface=RB).pack(side='right')
            dur = job.get('duration')
            if dur:
                tk.Label(inner_r, text='%ds' % int(dur), font=FONTS['caption'], bg=RB, fg=COLORS['fg_3']).pack(side='right', padx=(px(8), px(8)))
            st = job['status']
            Badge(inner_r, _BADGE_WORD.get(st, st.title()), _BADGE_TONE.get(st, 'neutral'), icon=_BADGE_ICON.get(st), surface=RB).pack(side='right', padx=(px(8), 0))
    def _uq_add():
        src = src_var.get().strip()
        outdir = outdir_var.get().strip()
        name = name_var.get().strip()
        t = out_type.get()
        _img_mode = t == 'pfs' and pfs_src_mode.get() == 'image'
        if _img_mode:
            if not src or (not os.path.isfile(src) or os.path.splitext(src)[1].lower() not in ('.exfat', '.ffpkg')):
                app_error(app, 'Source missing', 'Pick an existing .exfat or .ffpkg image.')
                return
        elif not src or not os.path.isdir(src):
            app_error(app, 'Source missing', 'Pick a valid game dump folder.')
            return
        if not outdir or not os.path.isdir(outdir):
            app_error(app, 'Output folder missing', 'Pick an output folder.')
            return
        if not _img_mode:
            try:
                if os.path.commonpath([os.path.abspath(src), os.path.abspath(outdir)]) == os.path.abspath(src):
                    app_error(app, 'Output folder inside the dump', 'The output folder is inside the game dump folder.\n\nPick an output folder OUTSIDE the dump — writing the image into the dump makes the packer try to include its own output file, which fails part-way through.')
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
        _final_ext = '.ffpfsc' if t == 'pfs' else ext
        _final_stem = stem
        _target = os.path.join(outdir, _final_stem + _final_ext)
        if os.path.exists(_target):
            _ow = app_choose3(app, 'Output already exists', '%s\n\nalready exists in the output folder.' % (_final_stem + _final_ext), ok_label='Overwrite', alt_label='Save numbered copy', cancel_label="Don't add")
            if _ow is None:
                return
            if _ow is False:
                _n = 2
                while os.path.exists(os.path.join(outdir, '%s (%d)%s' % (_final_stem, _n, _final_ext))):
                    _n += 1
                _final_stem = '%s (%d)' % (_final_stem, _n)
                stem = _final_stem
                name = _final_stem + ext
                try:
                    name_var.set(name)
                except Exception:
                    pass
        exfat_size = 'auto'
        if t == 'exfat' and img_size_mode.get() == 'custom':
            try:
                v = float(img_size_gb.get().strip())
                if v > 0:
                    exfat_size = v
            except Exception:
                exfat_size = 'auto'
        if not space_state.get('ok', True):
            lines = ['This build may not fit:', '']
            if space_state.get('same_drive') and space_state.get('tmp_need'):
                lines.append(f"Output + Temp share {space_state.get('out_drive') or 'one drive'!s} — needs ~{_fmt_b(space_state['out_need'] + space_state['tmp_need'])!s} combined, {_fmt_b(space_state['out_free']) if space_state.get('out_free') is not None else 'unknown'!s} free.")
            else:
                if space_state.get('out_free') is not None and space_state['out_free'] < space_state['out_need']:
                    lines.append(f"Output {space_state.get('out_drive') or ''!s} — needs ~{_fmt_b(space_state['out_need'])!s}, {_fmt_b(space_state['out_free'])!s} free.")
                if space_state.get('tmp_need') and (space_state.get('tmp_free') is not None and space_state['tmp_free'] < space_state['tmp_need']):
                    lines.append(f"Temp {space_state.get('tmp_drive') or ''!s} — needs ~{_fmt_b(space_state['tmp_need'])!s}, {_fmt_b(space_state['tmp_free'])!s} free.")
            lines += [*('', 'These are peak transient estimates — the build can fail part-way if the drive fills.', '', 'Add to the queue anyway?')]
            if not app_confirm(app, 'Low disk space', '\n'.join(lines)):
                return
        _job_temp = tempdir_var.get().strip() or str(app._settings.get('temp_dir') or '').strip()
        queue.append({'src': src, 'outdir': outdir, 'name': name, 'type': t, 'temp': _job_temp, 'via': pfs_via.get() if t == 'pfs' and not _img_mode else None, 'ppsa': (detected.get('title_id') or '').strip(), 'from_image': _img_mode, 'src_ext': os.path.splitext(src)[1].lower() if _img_mode else None, 'exfat_size': exfat_size if t == 'exfat' else None, 'backend': pfs_backend.get().strip().lower() or 'lazy', 'zlib_backend': pfs_zlib_backend.get().strip().lower() or 'zlib', 'status': 'queued', 'need_out': space_state.get('out_need', 0), 'need_tmp': space_state.get('tmp_need', 0), 'out_drive': space_state.get('out_drive', ''), 'tmp_drive': space_state.get('tmp_drive', '')})
        try:
            app._log(f"[QUEUE] captured src={src!s} outdir={outdir!s} temp={_job_temp or '(none)'!s} type={t!s}\n")
        except Exception:
            pass
        _uq_render()
    def _uq_load_folder_and_add(folder):
        try:
            if not folder or not os.path.isdir(folder):
                app_error(app, 'Build', 'That game folder no longer exists:\n\n%s' % folder)
                return False
            try:
                if out_type.get() == 'pfs' and pfs_src_mode.get() == 'image':
                    out_type.set('exfat')
            except Exception:
                pass
            target = folder
            try:
                from exfat_builder import resolve_game_root_deep
                _ll = getattr(app, '_log', lambda *_a, **_k: None)
                res = resolve_game_root_deep(folder, log=_ll)
                if res.get('root'):
                    target = res['root']
            except Exception:
                pass
            src_var.set(target)
            try:
                _on_source_changed()
            except Exception:
                pass
            _uq_add()
            return True
        except Exception as e:
            try:
                app_error(app, 'Build', str(e))
            except Exception:
                pass
            return False
    app._uq_load_folder_and_add = _uq_load_folder_and_add
    def _uq_remove_job(job):
        if job.get('status') != 'queued':
            return
        try:
            queue.remove(job)
        except ValueError:
            pass
        uq_state['sel'].pop(id(job), None)
        _uq_render()
    def _uq_move(i, delta):
        j = i + delta
        if not (0 <= i < len(queue)) or not (0 <= j < len(queue)):
            return
        # Only queued (not-yet-started) jobs can be reordered
        if queue[i].get('status') != 'queued' or queue[j].get('status') != 'queued':
            return
        queue[i], queue[j] = queue[j], queue[i]
        _uq_render()
    def _uq_remove_selected():
        victims = [j for j in queue
                   if j.get('status') == 'queued'
                   and id(j) in uq_state['sel']
                   and uq_state['sel'][id(j)].get()]
        for j in victims:
            try:
                queue.remove(j)
            except ValueError:
                pass
            uq_state['sel'].pop(id(j), None)
        _uq_render()
    def _uq_clear():
        if uq_state['running']:
            survivors = [j for j in queue if j.get('status') != 'queued']
            removed = [j for j in queue if j.get('status') == 'queued']
            for j in removed:
                uq_state['sel'].pop(id(j), None)
            queue[:] = survivors
            _uq_render()
            return
        queue.clear()
        uq_state['sel'].clear()
        summary_card.pack_forget()
        _uq_render()
    summary_card = Card(inner, padding=(20, 16))
    summary_card.configure(highlightbackground=COLORS['success'])
    swrap = tk.Frame(summary_card, bg=COLORS['bg_2'])
    swrap.pack(fill='x')
    sum_icon = StepIcon(swrap, '✓', size=24, bg=COLORS['bg_2'])
    sum_icon.pack(side='left', padx=(0, px(14)))
    scol = tk.Frame(swrap, bg=COLORS['bg_2'])
    scol.pack(side='left', fill='x', expand=True)
    tk.Label(scol, textvariable=sum_title_var, font=FONTS['subtitle'], bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(anchor='w')
    tk.Label(scol, textvariable=sum_jobs_var, font=FONTS['caption'], bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w').pack(anchor='w', pady=(px(2), 0))
    sgrid = tk.Frame(swrap, bg=COLORS['bg_2'])
    sgrid.pack(side='right')
    def _sum_render(fields):
        """Rebuild the summary stat columns from (label, value) pairs —
        only fields that apply to this run are shown."""
        for w in sgrid.winfo_children():
            w.destroy()
        for col, (label, value) in enumerate(fields):
            f = tk.Frame(sgrid, bg=COLORS['bg_2'])
            f.grid(row=0, column=col, sticky='w', padx=(px(24), 0))
            tk.Label(f, text=label.capitalize() if label.isupper() else label, font=FONTS['caption'], bg=COLORS['bg_2'],
                     fg=COLORS['fg_3'], anchor='w').pack(anchor='w')
            tk.Label(f, text=value, font=FONTS['body_strong'],
                     bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w').pack(
                         anchor='w', pady=(px(2), 0))
    def _set_buttons(enabled):
        st = 'normal' if enabled else 'disabled'
        try:
            btn_build.config(state=st)
        except Exception:
            pass
        try:
            btn_add.config(state='normal')
        except Exception:
            return
    def _maybe_build_ampr_index(src_root, fmt_label=''):
        """v3.6.3: if the dump contains fakelib/libSceAmpr.sprx, generate an
        ampr_emu.index at the app0 root (src_root) so the AMPR file resolver
        can find the game's files (case-insensitively) after conversion.

        src_root IS the app0 root (the dump folder). The index is written as
        a sibling of eboot.bin / sce_sys / fakelib and is then packed into
        the built image by the normal copy/pack step. Gated strictly on the
        fakelib sprx being present — never generated otherwise. Best-effort:
        a failure here is logged but does NOT abort the build (the game just
        won't have the resolver index).
        """
        try:
            sprx = os.path.join(src_root, 'fakelib', 'libSceAmpr.sprx')
            if not os.path.isfile(sprx):
                app._log('[AMPR] AMPR not detected — skipping index generation\n')
                return False
            app._log('[AMPR] AMPR support detected%s — generating ampr_emu.index…\n' % (' (%s)' % fmt_label if fmt_label else ''))
            from ui.build_ampr_index import build_index_local
            from pathlib import Path
            out = Path(src_root) / 'ampr_emu.index'
            if out.is_file():
                app._log('[AMPR] Existing ampr_emu.index found — refreshing it\n')
            _stats = {}
            rc = build_index_local(Path(src_root), out, True, stats_out=_stats)
            if rc == 0 and out.is_file():
                _files = _stats.get('files')
                if _files is not None:
                    app._log('[AMPR] AMPR index generated: %d files indexed\n' % _files)
                else:
                    app._log('[AMPR] AMPR index generated\n')
                return True
            app._log('[AMPR] WARNING: index generation returned %s — AMPR file resolution will not work; continuing the build without it (you can add it later with the AMPR Index Builder)\n' % rc)
            return False
        except Exception as e:
            try:
                app._log('[AMPR] WARNING: index generation error (%s) — AMPR file resolution will not work; continuing the build without it\n' % e)
            except Exception:
                pass
            return False
    def _dispatch_exfat(job, cb):
        try:
            _maybe_build_ampr_index(job['src'], 'exFAT')
            from exfat_builder import QueueItem
            job['_final_path'] = os.path.normpath(os.path.join(job['outdir'], job['name']))
            job['_inter_path'] = None
            job['_inter_is_artifact'] = False
            item = QueueItem(job['src'], job['outdir'], job['name'], game_title=os.path.basename(job['src'].rstrip('/\\')), size_override_gb=job.get('exfat_size') or 'auto')
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
                app.build_btn.config(state='disabled', text='Building…')
            except Exception:
                pass
            app._process_next([idx], 0)
        except Exception as e:
            try:
                app._log('[UNIBUILD] exFAT dispatch error: %s\n' % e)
            except Exception:
                pass
            cb(False)
            return
    def _dispatch_ffpkg(job, cb):
        try:
            _maybe_build_ampr_index(job['src'], 'ffpkg')
            if not getattr(app, '_ffpkg_ufs2_exe', None) or not os.path.isfile(app._ffpkg_ufs2_exe):
                from exfat_builder import extract_ufs2tool
                temp_dir = app._settings.get('temp_dir') or None
                app._ffpkg_ufs2_exe = extract_ufs2tool(temp_dir)
            job['_final_path'] = os.path.normpath(os.path.join(job['outdir'], job['name']))
            job['_inter_path'] = None
            job['_inter_is_artifact'] = False
            app._ffpkg_queue.append({'folder': job['src'], 'odir': job['outdir'], 'name': job['name'], 'status': 'waiting'})
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
            return
    def _choose_intermediate_dir(via, outdir, src_folder):
        """v3.6.4 (issue #31 / #25 / #45): decide WHERE the Stage-A
        intermediate image (.exfat / .ffpkg) is built.

        Historically the intermediate was always created in `outdir`, so a
        user with temp=fast-SSD and output=slow-HDD paid the full
        image-build write cost (the heaviest, longest-running stage) on the
        slow drive. This picks the configured temp/staging folder for the
        intermediate when it is BOTH safe and actually useful, and falls
        back to `outdir` (the original behaviour) otherwise.

        Returns the directory to build the intermediate in, or None to
        abort the whole job (only when output itself can't hold the image,
        i.e. the build is doomed either way and we should fail early with a
        clear message rather than midway).

        Safety rules (deliberately conservative):
          * temp must be set and an existing directory;
          * temp must be on a DIFFERENT physical drive than output (same
            drive = zero benefit, so don't add a redirect);
          * temp must have enough free space for the estimated image plus
            headroom;
          * for via='ffpkg' ONLY, temp must also support hard links — the
            ffpkg/PFS staging path uses os.link, which fails on exFAT/FAT32.
            This is the historical "staging must support hard links" trap;
            via='exfat' is a plain OSFMount/robocopy file write and is safe
            on any temp drive, so it skips this check.
        Stage B (compression) is untouched: it still reads the intermediate
        wherever it is and writes the final .ffpfsc to `outdir` (we pass it
        an explicit output path below), and its own space guard decides
        whether to stage the compressed file on temp — so the two stages
        cooperate without this function having to reason about Stage B.
        """
        def _log(msg):
            try:
                app._log('[PFS] Intermediate image staging: %s\n' % msg)
            except Exception:
                pass

        temp = (app._settings.get('temp_dir') or '').strip()

        # Estimate the intermediate size from the source dump. The exFAT
        # intermediate is built 'auto' (smallest safe ~= source + margin),
        # so the source folder size is a sound, conservative basis.
        try:
            est = app._get_folder_size(src_folder) if src_folder else 0
        except Exception:
            est = 0
        # Headroom: image carries filesystem + allocator overhead on top of
        # the raw file bytes, so require comfortably more than the estimate.
        need_temp = int(est * 1.3) if est else 0
        # Output is only considered "doomed" (early-fail) if it can't even
        # hold roughly the raw image — a tighter bound than the temp guard
        # so we never false-abort a build output could actually fit.
        need_out_min = int(est * 1.05) if est else 0

        def _free(path):
            try:
                return _pfs_free_space(path)
            except Exception:
                return None

        # ── If temp can't be used, fall back to outdir (and early-fail only
        #    when outdir itself clearly lacks space). ──
        def _fallback(reason):
            _log('temp not used because %s.' % reason)
            of = _free(outdir)
            if est and (of is not None) and of < need_out_min:
                _log('falling back to output folder, but the output drive '
                     'has only %s free and the image needs about %s \u2014 '
                     'not enough space. Aborting before build.'
                     % (app._fmt_size(of), app._fmt_size(est)))
                return None
            _log('falling back to output folder %s.' % outdir)
            return outdir

        if not temp:
            return _fallback('no temp/staging folder is configured')
        if not os.path.isdir(temp):
            return _fallback('the configured temp folder does not exist (%s)'
                             % temp)

        # Same-drive check — no benefit redirecting within one physical drive.
        try:
            same_drive = (os.path.splitdrive(os.path.abspath(temp))[0].lower()
                          == os.path.splitdrive(os.path.abspath(outdir))[0].lower())
        except Exception:
            same_drive = True
        if same_drive:
            return _fallback('temp and output are on the same drive')

        # Conservative space guard on temp.
        if not est:
            return _fallback('the source image size could not be estimated, '
                             'so temp free space cannot be verified')
        tf = _free(temp)
        if tf is None:
            return _fallback('temp free space could not be read')
        if tf < need_temp:
            return _fallback('temp has %s free but the image needs about %s '
                             '(incl. headroom)'
                             % (app._fmt_size(tf), app._fmt_size(need_temp)))

        # ffpkg-only hard-link safety (exFAT path is a plain file write).
        if via == 'ffpkg':
            try:
                hl_ok = _pfs_supports_hardlinks(temp)
            except Exception:
                hl_ok = False
            if not hl_ok:
                return _fallback('the ffpkg intermediate needs a temp drive '
                                 'that supports hard links and this one does '
                                 'not (exFAT/FAT32?)')

        try:
            _fs = _pfs_fs_name(temp)
        except Exception:
            _fs = 'unknown'
        _log('using temp folder %s (%s, %s free).'
             % (temp, _fs, app._fmt_size(tf)))
        return temp
    def _dispatch_pfs(job, cb):
        via = job.get('via') or 'exfat'
        _via_label = 'ffpkg' if via == 'ffpkg' else 'exfat'
        try:
            app._log('[PFS] Intermediate selected: %s\n' % _via_label)
            app._log('[PFS] Pipeline: Dump → %s → ffpfsc\n' % ('ffpkg' if via == 'ffpkg' else 'exFAT'))
        except Exception:
            pass
        final = job['name']
        stem = final[:-len('.ffpfsc')] if final.lower().endswith('.ffpfsc') else final
        inter_ext = '.exfat' if via == 'exfat' else '.ffpkg'
        _ppsa = (job.get('ppsa') or '').strip()
        if _ppsa:
            try:
                from exfat_builder import sanitize_filename
                _ppsa = sanitize_filename(_ppsa) or _ppsa
            except Exception:
                pass
        inter_stem = _ppsa if _ppsa else stem
        inter_name = inter_stem + inter_ext
        inter_dir = _choose_intermediate_dir(via, job['outdir'], job['src'])
        if inter_dir is None:
            try:
                app._set_status('PFS build aborted — not enough disk space (see log)', '#d9534f')
            except Exception:
                pass
            cb(False)
            return
        inter_path = os.path.normpath(os.path.join(inter_dir, inter_name))
        final_path = os.path.normpath(os.path.join(job['outdir'], stem + '.ffpfsc'))
        job['_inter_path'] = inter_path
        job['_final_path'] = final_path
        job['_inter_is_artifact'] = True
        inter_job = {'src': job['src'], 'outdir': inter_dir, 'name': inter_name}
        def _after_build(ok):
            if not ok:
                cb(False)
                return
            if ok == 'warn':
                # Do not compress an image that verification flagged; keep
                # it so nothing is lost (issue #86).
                try:
                    app._log('[UNIBUILD] PFS: the intermediate image was '
                             'flagged by verification, so it was not '
                             'compressed. It is kept at: %s\n' % inter_path)
                except Exception:
                    pass
                cb('warn')
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
            try:
                app._logts('Intermediate %s image ready.' % ('ffpkg' if via == 'ffpkg' else 'exFAT'))
            except Exception:
                pass
            uq_status.set('Starting compression for %s…' % stem)
            _isz = job.get('_inter_size', 0)
            try:
                app._log('Source image: %s\n' % app._fmt_size(_isz))
                app._log('Preparing PFS compression… this can take several minutes on large images.\n')
                app._logts('Phase 6/6 — Starting Compression')
                app._logts('Starting mkpfs...')
            except Exception:
                pass
            try:
                app._uq_heartbeat_start(5, src_size=_isz)
            except Exception:
                pass
            def _after_convert():
                try:
                    app._uq_heartbeat_stop()
                except Exception:
                    pass
                # The pipeline records whether the .ffpfsc came out good. A
                # leftover intermediate it could not delete (locked by AV or
                # Explorer) does not make a finished .ffpfsc a failure, and
                # must never get it deleted by the failure cleanup (issue
                # audit H1).
                _res = app.pipeline_convert_result(final_path)
                if _res is None:
                    ok2 = os.path.isfile(final_path) and not os.path.isfile(inter_path)
                else:
                    ok2 = bool(_res) and os.path.isfile(final_path)
                if ok2 and os.path.isfile(inter_path) and job.get('_inter_is_artifact'):
                    try:
                        app._log('[UNIBUILD] .ffpfsc finished; the intermediate '
                                 'image could not be removed and was left at: '
                                 '%s\n' % inter_path)
                    except Exception:
                        pass
                if ok2:
                    _uq_set_progress(100, 'Converted to .ffpfsc', 'done')
                    try:
                        fsz = os.path.getsize(final_path)
                        isz = job.get('_inter_size', 0)
                        job['out_path'] = final_path
                        job['space_saved'] = max(0, isz - fsz)
                        job['compression'] = (1 - fsz / isz) * 100 if isz else 0
                    except Exception:
                        pass
                cb(ok2)
            try:
                app._pipeline_convert_then_next(inter_path, _after_convert, explicit_output_path=final_path, allow_experimental=True, backend=job.get('backend') or 'lazy', zlib_backend=job.get('zlib_backend') or 'zlib', cancel_check=(lambda: not uq_state.get('running', True)), notify_on_cancel=True)
            except Exception as e:
                try:
                    app._log('[UNIBUILD] PFS convert error: %s\n' % e)
                except Exception:
                    pass
                cb(False)
                return
        if via == 'ffpkg':
            _dispatch_ffpkg(inter_job, _after_build)
            return
        _dispatch_exfat(inter_job, _after_build)
    def _pfs_fs_name(path):
        """Best-effort filesystem name ('NTFS', 'exFAT', 'FAT32', ...) for the
        drive holding `path`. Windows-only via GetVolumeInformationW; returns
        'unknown' elsewhere or on failure."""
        try:
            if os.name != 'nt':
                return 'unknown'
            import ctypes as _ct
            drive = os.path.splitdrive(os.path.abspath(path))[0]
            if not drive:
                return 'unknown'
            root = drive + '\\'
            fs_buf = _ct.create_unicode_buffer(256)
            ok = _ct.windll.kernel32.GetVolumeInformationW(
                root, None, 0, None, None, None, fs_buf, 256)
            return (fs_buf.value or 'unknown') if ok else 'unknown'
        except Exception:
            return 'unknown'
    def _pfs_supports_hardlinks(directory):
        """True if `directory` is on a filesystem that supports hard links
        (NTFS yes; exFAT / FAT32 no). mkpfs stages its source via os.link,
        and we use the same primitive for zero-copy staging."""
        try:
            import tempfile as _tf
            with _tf.TemporaryDirectory(dir=directory) as _d:
                _a = os.path.join(_d, 'a')
                _b = os.path.join(_d, 'b')
                with open(_a, 'wb'):
                    pass
                try:
                    os.link(_a, _b)
                    return True
                except OSError:
                    return False
        except Exception:
            return False
    def _pfs_free_space(path):
        import shutil as _sh2
        try:
            d = path if os.path.isdir(path) else os.path.dirname(path) or '.'
            return _sh2.disk_usage(d).free
        except Exception:
            return None
    def _pfs_copy_with_progress(src, dst, total, should_cancel):
        """Chunked copy that reports byte progress / speed / ETA through the
        unified progress widgets and the log, and honours cancellation.
        Returns True on success, False on cancel/error (dst removed)."""
        import time as _t
        CHUNK = 8 * 1024 * 1024  # 8 MiB
        copied = 0
        t0 = _t.monotonic()
        last_ui = 0.0
        try:
            with open(src, 'rb') as fi, open(dst, 'wb') as fo:
                while True:
                    if should_cancel():
                        raise InterruptedError('cancelled')
                    buf = fi.read(CHUNK)
                    if not buf:
                        break
                    fo.write(buf)
                    copied += len(buf)
                    now = _t.monotonic()
                    if now - last_ui >= 0.25:
                        last_ui = now
                        elapsed = max(1e-6, now - t0)
                        rate = copied / elapsed
                        pct = (copied / total * 100.0) if total else 0.0
                        eta = ((total - copied) / rate) if (rate > 0 and total) else 0
                        eta_s = '%dm %02ds' % (int(eta) // 60, int(eta) % 60)
                        detail = ('%s / %s  \u00b7  %s/s  \u00b7  ETA %s'
                                  % (app._fmt_size(copied), app._fmt_size(total),
                                     app._fmt_size(int(rate)), eta_s))
                        _uq_set_progress(pct, 'Copying source image for safe staging', detail)
                fo.flush()
                try:
                    os.fsync(fo.fileno())
                except Exception:
                    pass
            _uq_set_progress(100, 'Copying source image for safe staging', 'staged')
            return True
        except InterruptedError:
            app._log('[UNIBUILD] PFS(image): staging copy cancelled.\n')
        except Exception as e:
            app._log('[UNIBUILD] PFS(image): staging copy failed: %s\n' % e)
        try:
            if os.path.exists(dst):
                os.remove(dst)
        except Exception:
            pass
        return False
    def _dispatch_pfs_from_image(job, cb):
        import shutil as _sh
        import tempfile as _tf
        src_image = job['src']
        src_ext = job.get('src_ext') or (os.path.splitext(src_image)[1].lower() or '.exfat')
        final = job['name']
        stem = final[:-len('.ffpfsc')] if final.lower().endswith('.ffpfsc') else final
        outdir = job['outdir']
        final_path = os.path.normpath(os.path.join(outdir, stem + '.ffpfsc'))
        job['_final_path'] = final_path
        job['_inter_path'] = None
        job['_inter_is_artifact'] = False
        src_dir = os.path.dirname(os.path.abspath(src_image))
        src_name = os.path.basename(src_image)
        src_stem = os.path.splitext(src_name)[0]
        ascii_stem = src_stem.encode('ascii', 'ignore').decode('ascii').strip().strip('.') or 'image'
        try:
            src_size = os.path.getsize(src_image)
        except Exception:
            src_size = 0
        job['_inter_size'] = src_size
        def _should_cancel():
            return not uq_state.get('running', True)
        def _finish_after_convert(stage_dir, produced_ffpfsc):
            def _after_convert():
                try:
                    app._uq_heartbeat_stop()
                except Exception:
                    pass
                _res = app.pipeline_convert_result(produced_ffpfsc)
                ok2 = os.path.isfile(produced_ffpfsc) and _res is not False
                if _res is False and os.path.isfile(produced_ffpfsc):
                    # Written but rejected (e.g. mkpfs's check failed):
                    # never moved into place as a finished image.
                    try:
                        os.remove(produced_ffpfsc)
                        app._log('[UNIBUILD] Removed the rejected .ffpfsc: '
                                 '%s\n' % produced_ffpfsc)
                    except Exception:
                        pass
                out_final = final_path
                if ok2:
                    try:
                        out_final = app._move_final(produced_ffpfsc, final_path, log=(lambda s: app._log('[UNIBUILD] %s\n' % s)), progress=_uq_set_progress)
                    except Exception as e:
                        app._log(f"[UNIBUILD] PFS(image): could not move to {final_path!s} ({e!s}) — keeping the produced file at {produced_ffpfsc!s}.\n")
                        out_final = produced_ffpfsc
                ok2 = os.path.isfile(out_final)
                if ok2:
                    try:
                        fsz = os.path.getsize(out_final)
                        isz = job.get('_inter_size', 0)
                        job['out_path'] = out_final
                        job['space_saved'] = max(0, isz - fsz)
                        job['compression'] = (1 - fsz / isz) * 100 if isz else 0
                    except Exception:
                        pass
                if stage_dir:
                    try:
                        _inside = os.path.normcase(os.path.abspath(out_final)).startswith(os.path.normcase(os.path.abspath(stage_dir)) + os.sep)
                    except Exception:
                        _inside = True
                    if _inside:
                        app._log('[UNIBUILD] PFS(image): finished file is inside the staging folder — leaving %s in place.\n' % stage_dir)
                    else:
                        _sh.rmtree(stage_dir, ignore_errors=True)
                cb(bool(ok2))
            return _after_convert
        def _start_pack(pack_input, stage_dir, explicit_out):
            uq_status.set('Starting compression for %s…' % os.path.basename(src_image))
            _isz = job.get('_inter_size', 0)
            try:
                app._log('Source image: %s\n' % app._fmt_size(_isz))
                app._log('Preparing PFS compression… this can take several minutes on large images.\n')
                app._logts('Phase 6/6 — Starting Compression')
                app._logts('Starting mkpfs...')
            except Exception:
                pass
            try:
                app._uq_heartbeat_start(5, src_size=_isz)
            except Exception:
                pass
            try:
                app._pipeline_convert_then_next(pack_input, _finish_after_convert(stage_dir, explicit_out), delete_source_on_success=False, explicit_output_path=explicit_out, allow_experimental=True, backend=job.get('backend') or 'lazy', zlib_backend=job.get('zlib_backend') or 'zlib', cancel_check=_should_cancel, notify_on_cancel=True)
            except Exception as e:
                try:
                    app._uq_heartbeat_stop()
                except Exception:
                    pass
                app._log('[UNIBUILD] PFS(image) pack error: %s\n' % e)
                if stage_dir:
                    _sh.rmtree(stage_dir, ignore_errors=True)
                cb(False)
                return
        def _is_ascii(s):
            try:
                s.encode('ascii')
                return True
            except Exception:
                return False
        def _prep_worker():
            # Runs OFF the Tk thread. All UI touches go via app.after / the
            # thread-safe _uq_set_progress.
            #
            # v3.6.2: existing-image -> .ffpfsc no longer stages a full copy.
            # mkpfs `pack file` reads the source in place and writes a
            # separate .ffpfsc, so for normal (ASCII) paths we pass the
            # ORIGINAL straight to mkpfs and write the output directly to the
            # chosen folder (explicit_output_path). delete_source_on_success
            # is False, so the original is never touched. A narrow link/copy
            # fallback exists ONLY for non-ASCII source paths (mkpfs can choke
            # on those); it is never used for ASCII paths.
            app.after(0, lambda: uq_status.set(
                'Preparing source image — %s…' % src_name))
            app.after(0, _uq_set_progress, 0, 'Preparing source image', '')

            # Does the source path contain non-ASCII characters? Only then do
            # we consider a staged ASCII-safe alias.
            path_is_ascii = _is_ascii(src_image)

            if path_is_ascii:
                # ── Normal path: use the original directly, no staging. ──
                app._log('[UNIBUILD] PFS(image): Using existing image '
                         'directly. No staging copy required.\n')
                app._log('[UNIBUILD] PFS(image): Source: %s\n' % src_image)
                app._log('[UNIBUILD] PFS(image): Output: %s\n' % final_path)
                app._log('[UNIBUILD] PFS(image): Original source will not be '
                         'modified or deleted.\n')
                app.after(0, _start_pack, src_image, None, final_path)
                return

            # ── Non-ASCII fallback only. ──
            app._log('[UNIBUILD] PFS(image): Non-ASCII source path detected. '
                     'Using ASCII-safe staging fallback.\n')
            stage_dir = None
            staged = None

            # 1) Try a zero-copy hard link to an ASCII-safe name on the
            #    source drive.
            if _pfs_supports_hardlinks(src_dir):
                stage_dir = os.path.join(src_dir, '.pfs_stage')
                try:
                    if os.path.isdir(stage_dir):
                        _sh.rmtree(stage_dir, ignore_errors=True)
                    os.makedirs(stage_dir, exist_ok=True)
                    staged = os.path.join(stage_dir, ascii_stem + src_ext)
                    os.link(src_image, staged)
                    app._log('[UNIBUILD] PFS(image): staged a hard link '
                             '(instant, no copy, original untouched).\n')
                except Exception as e:
                    app._log('[UNIBUILD] PFS(image): hard link failed (%s); '
                             'will copy as a last resort.\n' % e)
                    if stage_dir:
                        _sh.rmtree(stage_dir, ignore_errors=True)
                    stage_dir = None
                    staged = None

            # 2) Last resort: copy (with progress) to an ASCII-safe name on a
            #    hard-link-capable drive with room.
            if staged is None:
                _src_fs = _pfs_fs_name(src_dir)
                app._log('[UNIBUILD] PFS(image): copying to an ASCII-safe '
                         'staging name (source fs: %s).\n' % _src_fs)
                cand_bases = []
                tv = (app._settings.get('temp_dir') or '').strip()
                if tv:
                    cand_bases.append(tv)
                cand_bases.append(outdir)
                cand_bases.append(_tf.gettempdir())
                dest_base = None
                for cand in cand_bases:
                    try:
                        if not cand or not os.path.isdir(cand):
                            continue
                        free = _pfs_free_space(cand) or 0
                        if free > src_size + (256 * 1024 * 1024):
                            dest_base = cand
                            break
                    except Exception:
                        pass
                if dest_base is None:
                    dest_base = os.path.dirname(src_image)
                stage_dir = _tf.mkdtemp(prefix='.pfs_stage_', dir=dest_base)
                staged = os.path.join(stage_dir, ascii_stem + src_ext)
                app._log('[UNIBUILD] PFS(image): Source: %s\n' % src_image)
                app._log('[UNIBUILD] PFS(image): Stage:  %s\n' % staged)
                app.after(0, lambda: uq_status.set(
                    'Copying source image for safe staging on %s…' % dest_base))
                ok_copy = _pfs_copy_with_progress(
                    src_image, staged, src_size, _should_cancel)
                if not ok_copy:
                    _sh.rmtree(stage_dir, ignore_errors=True)
                    app.after(0, lambda: cb(False))
                    return

            app.after(0, _start_pack, staged, stage_dir, final_path)

        threading.Thread(target=_prep_worker, daemon=True).start()
    def _uq_dispatch(job, cb):
        t = job['type']
        try:
            _pre_fp = os.path.normpath(os.path.join(job['outdir'], job['name']))
            if os.path.isfile(_pre_fp):
                _pre_st = os.stat(_pre_fp)
                job['_final_preexists'] = _pre_fp, _pre_st.st_size, _pre_st.st_mtime
            else:
                job['_final_preexists'] = None
        except Exception:
            job['_final_preexists'] = None
        try:
            _set_phase_mode(t == 'pfs')
        except Exception:
            pass
        if t == 'exfat':
            _dispatch_exfat(job, cb)
            return
        if t == 'ffpkg':
            _dispatch_ffpkg(job, cb)
            return
        if t == 'pfs':
            try:
                _be = (job.get('backend') or 'lazy').strip().lower()
                app._log('[UNIBUILD] PFS convert backend for this job: %s\n' % ('Lazy_MkPFS (default)' if _be == 'lazy' else 'MkPFS (PSBrew)'))
            except Exception:
                pass
            if job.get('from_image'):
                _dispatch_pfs_from_image(job, cb)
                return
            _dispatch_pfs(job, cb)
            return
        cb(None)
    def _uq_cleanup_job(job, reason='failed'):
        # Farewell Edition (issue #86): this used to delete whatever the job
        # had produced on ANY failure - including a finished exFAT image
        # whose verification merely raised a doubt, and the verified
        # intermediate image when only the later .ffpfsc compression
        # failed. Completed images are now always kept. What is still
        # removed: the intermediate when the user cancelled, and a
        # half-written .ffpfsc this run created.
        removed = []
        kept = []
        try:
            ip = job.get('_inter_path')
            if ip and (job.get('_inter_is_artifact') and os.path.isfile(ip)):
                if reason == 'cancelled':
                    os.remove(ip)
                    removed.append(ip)
                else:
                    kept.append(ip)
        except Exception:
            pass
        if job.get('type') in ('exfat', 'ffpkg') and reason != 'cancelled':
            # The builders already delete genuinely partial images
            # themselves; anything still here was built to the end.
            fp = job.get('_final_path') or job.get('out_path')
            if fp and os.path.isfile(fp):
                kept.append(fp)
            fp = None
        else:
            fp = job.get('_final_path') or job.get('out_path')
        for k in kept:
            try:
                app._log('[UNIBUILD] %s — kept %s for inspection (not deleted).\n'
                         % (reason, k))
            except Exception:
                pass
        try:
            if fp and (os.path.isfile(fp) and job.get('status') in (None, 'running', 'failed', 'cancelled')):
                _keep = False
                _pre = job.get('_final_preexists')
                if _pre:
                    try:
                        _pp, _psz, _pmt = _pre
                        if os.path.normcase(_pp) == os.path.normcase(os.path.normpath(fp)):
                            _st = os.stat(fp)
                            _keep = _st.st_size == _psz and _st.st_mtime == _pmt
                    except Exception:
                        _keep = False
                if _keep:
                    try:
                        app._log(f"[UNIBUILD] {reason!s} cleanup — kept {os.path.basename(fp)!s} (pre-existing file from an earlier build; this run never wrote it).\n")
                    except Exception:
                        pass
                else:
                    os.remove(fp)
                    removed.append(fp)
        except Exception:
            pass
        if removed:
            try:
                app._log(f"[UNIBUILD] {reason!s} cleanup — removed: {', '.join(os.path.basename(r) for r in removed)!s}\n")
            except Exception:
                pass
        return removed
    def _uq_done(job, ok):
        try:
            job['duration'] = max(0, time.time() - job.get('_t0', time.time()))
        except Exception:
            pass
        if job['type'] != 'pfs' and 'out_path' not in job:
            job['out_path'] = os.path.join(job['outdir'], job['name'])
        if uq_state.get('cancel') and not ok:
            # Only work that did not finish is cleaned up on cancel. An
            # exFAT/ffpkg build cannot be interrupted mid-copy, so it often
            # completes after Cancel was pressed: that image is a finished
            # result and is kept (audit H4).
            job['status'] = 'cancelled'
            _uq_cleanup_job(job, reason='cancelled')
        elif uq_state.get('cancel') and ok:
            job['status'] = 'warn' if ok == 'warn' else 'done'
            try:
                app._log('[UNIBUILD] %s finished before the cancel took '
                         'effect; the image was kept.\n' % job.get('name', ''))
            except Exception:
                pass
        elif ok is None:
            job['status'] = 'skipped'
        elif ok == 'warn':
            # Built, but verification raised a doubt: keep everything.
            job['status'] = 'warn'
            try:
                app._log('[UNIBUILD] %s finished with a verification warning; '
                         'the image was kept — check it before use.\n'
                         % job.get('name', ''))
            except Exception:
                pass
        elif ok:
            job['status'] = 'done'
        else:
            job['status'] = 'failed'
            _uq_cleanup_job(job, reason='failed')
        app._unified_job_done = None
        if uq_state.get('current') is job:
            uq_state['current'] = None
        _uq_render()
        if uq_state.get('cancel'):
            _uq_finish()
            return
        parent.after(60, _uq_run_next)
    def _uq_run_next():
        nxt = next((j for j in queue if j['status'] == 'queued'), None)
        if nxt is None:
            uq_state['current'] = None
            _uq_finish()
            return
        nxt['status'] = 'running'
        nxt['_t0'] = time.time()
        uq_state['current'] = nxt
        try:
            _pos = queue.index(nxt) + 1
        except ValueError:
            _pos = 1
        _remaining = [q for q in queue if q['status'] == 'queued']
        _nxt_name = _remaining[0]['name'] if _remaining else None
        _aft_name = _remaining[1]['name'] if len(_remaining) > 1 else None
        _q_bits = ['Building %d/%d  ·  %s  (%s)…' % (_pos, len(queue), nxt['name'], _TYPE_LABEL[nxt['type']])]
        if _nxt_name:
            _q_bits.append('Next: %s' % _nxt_name)
        if _aft_name:
            _q_bits.append('After: %s' % _aft_name)
        uq_status.set('     •     '.join(_q_bits))
        _uq_set_progress(0, 'Starting %s…' % nxt['name'], '')
        _uq_render()
        _uq_dispatch(nxt, lambda ok, j=nxt: _uq_done(j, ok))
    def _uq_run_idx(_i=0):
        _uq_run_next()
    def _uq_finish():
        uq_state['running'] = False
        uq_state['current'] = None
        try:
            _uq_show_cancel(False)
        except Exception:
            pass
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
        app._unified_status_card_show(False)
        _uq_render()

        done_jobs = [j for j in queue if j['status'] == 'done']
        done = len(done_jobs)
        failed = sum(1 for j in queue if j['status'] == 'failed')
        cancelled = sum(1 for j in queue if j['status'] == 'cancelled')
        total_dur = sum(j.get('duration', 0) for j in queue)

        line = '%d job%s finished  \u00b7  %d failed' % (
            done, '' if done == 1 else 's', failed)
        if cancelled:
            line += '  \u00b7  %d cancelled' % cancelled
        warned = sum(1 for j in queue if j['status'] == 'warn')
        if warned:
            line += '  \u00b7  %d kept for checking' % warned
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

        # Headline: cancelled wins over failed wins over complete
        if cancelled:
            sum_title_var.set('BUILD CANCELLED')
            sum_icon.config(text='\u2715', fg=COLORS['warn'])
            _edge = COLORS['warn']
        elif failed:
            sum_title_var.set('BUILD FINISHED \u2014 %d ERROR%s'
                              % (failed, '' if failed == 1 else 'S'))
            sum_icon.config(text='\u26a0', fg=COLORS['warn'])
            _edge = COLORS['warn']
        else:
            sum_title_var.set('BUILD COMPLETE')
            sum_icon.config(text='\u2713', fg=COLORS['success'])
            _edge = COLORS['success']
        summary_card.config(highlightbackground=_edge)
        summary_card.pack(fill='x', padx=24, pady=(0, 16))

        if cancelled:
            _toast(app, 'Build cancelled',
                   '%d finished' % done,
                   '%d cancelled' % cancelled, ok=False)
            return
        _toast(app, 'Build complete' if not failed else 'Build finished',
               '%d job%s finished' % (done, '' if done == 1 else 's'),
               '%d failed' % failed, ok=(failed == 0))

        # Unified-build shutdown hook (Settings > Shutdown on unified build)
        try:
            app._trigger_shutdown('unibuild')
        except Exception:
            pass
    def _uq_build_all():
        if uq_state['running']:
            return
        if not queue:
            app_info(app, 'Build queue', 'Add some jobs first.')
            return
        if getattr(app, '_building', False) or getattr(app, '_ffpkg_building', False):
            app_warning(app, 'Busy', 'A build is already running on another tab — let it finish first.')
            return
        if not any(j['status'] in ('queued', 'failed', 'skipped') for j in queue):
            app_info(app, 'Build queue', 'Nothing left to build.')
            return
        try:
            import shutil as _sh
            per_drive = {}
            for j in queue:
                if j['status'] not in ('queued', 'failed', 'skipped'):
                    continue
                od = (j.get('out_drive') or '').upper()
                if od and j.get('need_out'):
                    d = per_drive.setdefault(od, {'need': 0, 'tmp_max': 0, 'path': j['outdir']})
                    d['need'] += int(j['need_out'])
                td = (j.get('tmp_drive') or '').upper()
                if td and j.get('need_tmp'):
                    d = per_drive.setdefault(td, {'need': 0, 'tmp_max': 0, 'path': j['outdir']})
                    d['tmp_max'] = max(d['tmp_max'], int(j['need_tmp']))
            shortfalls = []
            for drv, d in per_drive.items():
                need = d['need'] + d['tmp_max']
                if not need:
                    continue
                try:
                    free = _sh.disk_usage(drv + '\\').free
                except Exception:
                    try:
                        free = _sh.disk_usage(d['path']).free
                    except Exception:
                        continue
                if free < need:
                    shortfalls.append(f"  {drv!s} — may need up to ~{_fmt_b(need)!s}, {_fmt_b(free)!s} free")
            if shortfalls and not app_confirm(app, 'Low disk space', 'The queued builds may not fit on these drive(s):\n\n' + '\n'.join(shortfalls) + '\n\nEstimates are conservative peak figures; a build that runs out of space fails part-way through.\n\nStart building anyway?'):
                return
        except Exception:
            pass
        for j in queue:
            if j['status'] in ('failed', 'skipped'):
                j['status'] = 'queued'
                j.pop('duration', None)
        try:
            from exfat_builder import save_settings
            _dirty = False
            tv = tempdir_var.get().strip()
            if tv != (app._settings.get('temp_dir') or ''):
                app._settings['temp_dir'] = tv
                _dirty = True
            ov = outdir_var.get().strip()
            if ov != (app._settings.get('output_dir') or ''):
                app._settings['output_dir'] = ov
                _dirty = True
            if _dirty:
                save_settings(app._settings)
        except Exception:
            pass
        summary_card.pack_forget()
        uq_state['running'] = True
        uq_state['current'] = None
        uq_state['cancel'] = False
        uq_state['added'] = []
        uq_state['added_ffpkg'] = []
        app._unified_active = True
        _set_buttons(False)
        try:
            _uq_show_cancel(True)
        except Exception:
            pass
        _uq_progress_show(True)
        app._unified_status_card_show(True)
        _uq_render()
        _uq_run_next()
    _restyle_tiles()
    _refresh_via()
    _refresh_size_row()
    _refresh_strip()
    _refresh_ready()
