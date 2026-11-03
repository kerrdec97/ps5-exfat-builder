"""
ui/tab_files.py — exFAT File Manager tab (v2.9.0 redesign — Variant A).

Step 50 (v2.9.0): full redesign per design_handoff_exfat_advanced/.
Replaces the flat list with a split layout:

    [ page head: 📁 exFAT File Manager  + subtitle              ]
    [ mounted-image card: 📀 PPSA + title + path + drive stats  ]
    ┌─ list column (1fr) ─────────────────┬─ inspector (288px) ─┐
    │ toolbar: breadcrumb + grouped btns  │ Inspector           │
    │ ┌─ Treeview ─────────────────────┐  │ ⬡ big icon          │
    │ │ Name | Type | Size | Modified │  │ filename            │
    │ │ ……                            │  │ size                │
    │ └────────────────────────────────┘  │ PATH/TYPE/MODIFIED/ │
    │ footer: 1 selected · 519 MB         │ SHA-256             │
    │                                     │ First 64 bytes hex  │
    │                                     │ [Replace][Extract]🗑│
    └─────────────────────────────────────┴─────────────────────┘

Backwards-compat: the existing `app._fm_listbox`-style API is preserved
via a `ListboxProxy` shim that wraps the Treeview. The ~16 callsites in
exfat_builder.py keep using `.curselection()`, `.nearest(y)`,
`.selection_set(i)`, `.delete(0, 'end')`, `.insert('end', text)` — they
all route through the proxy. `_fm_entries` indexing stays in lockstep
with Treeview row order.
"""

import os
import time
import tkinter as tk
from ui.shared.drives import used_letter_mask
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from tkinter import ttk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _
# Underscore-prefixed names aren't pulled in by the star-import, and
# subprocess/tempfile are submodule imports made inside exfat_builder
# (not re-exported either). Pull them in explicitly here so the
# apply-backport helpers below have what they need.
from exfat_builder import _NO_WIN_FLAGS, extract_ufs2tool
from ui.shared.app_dialog import app_warning, app_error, app_confirm, app_success
import subprocess
import tempfile

# Design-system components and the editor workspace building blocks
# shared with the ffpkg editor (summary bar, command bar, table and
# inspector cards, dialog skeleton). Imported after the star import so
# nothing from exfat_builder's namespace can shadow them.
from tkinter_theme import px
from ui.shared.widgets import (PageHeader, Button, IconButton,
                               EmptyState as DSEmptyState)
from ui.tab_ffpkg_edit import (
    _INSPECTOR_WIDTH, _style_sel_pill, _kind_icon, _kind_fg, _tree_icon,
    _ImageSummary, _CommandBar, _build_file_table_card,
    _build_inspector_card, _inspector_row, _insp_set_icon, _hex_well,
    _place_overlay, _hex_lines, _elide_middle,
    _dialog_shell, _dialog_place, _dialog_button, _choice_row)


def _classify(name, is_dir):
    """Map a filename to a (kind, label) pair for the Type column."""
    if is_dir:
        return ('folder', 'Folder')
    lname = name.lower()
    if lname in ('eboot.bin', 'sidbase.bin') or lname.endswith('.elf'):
        return ('exec', 'Binary')
    if lname.endswith('.log'):
        return ('log', 'Log')
    if lname.endswith(('.txt', '.json', '.xml', '.ini')):
        return ('text', 'Text')
    if lname.startswith('sce_') or lname.endswith(('.sprx', '.xpps')):
        return ('data', 'Data')
    if lname.endswith(('.cfg', '.conf')):
        return ('config', 'Config')
    if lname.endswith(('.bin', '.dat', '.pkg', '.exfat')):
        return ('binary', 'Binary')
    return ('unknown',
            name.split('.')[-1].upper() if '.' in name else 'File')


def _format_size(b):
    if not b:
        return '0 B'
    for unit, div in (('GB', 1024**3), ('MB', 1024**2), ('KB', 1024)):
        if b >= div:
            if div >= 1024**2:
                return '%.2f %s' % (b / div, unit)
            return '%d %s' % (b // div, unit)
    return '%d B' % b


# ─────────────────────────────────────────────────────────────────────
# ListboxProxy — Listbox API over a ttk.Treeview
# ─────────────────────────────────────────────────────────────────────
class ListboxProxy:
    """Forwards Listbox-style calls onto a Treeview. Treeview row IIDs
    are generated as zero-padded integers ('00000', '00001', ...) which
    map 1:1 to indices the old code expects."""

    def __init__(self, tree):
        self._tree = tree
        self._count = 0

    # Insert / delete
    def insert(self, _where, text):
        iid = '%05d' % self._count
        self._count += 1
        s = text.lstrip()
        # Strip any leading non-ASCII glyph the caller added — we'll
        # pick our own icon based on the file classification.
        if s and ord(s[0]) > 127:
            s = s[1:].lstrip()
        size_str = ''
        parts = s.rsplit(None, 2)
        if (len(parts) >= 2
                and parts[-1] in ('B', 'KB', 'MB', 'GB', 'TB')
                and any(c.isdigit() for c in parts[-2])):
            size_str = parts[-2] + ' ' + parts[-1]
            s = ' '.join(parts[:-2]) if len(parts) > 2 else parts[0]
        name = s.strip()
        # Detect parent-dir or folder from the original icon glyph
        is_dir = (name == '..' or text.lstrip().startswith(
            ('\U0001f4c1', '\U0001f4c2')))
        kind, type_label = _classify(name, is_dir)
        icon = 'folder_out' if name == '..' else _kind_icon(kind)
        # The tree column shows icon + name; `values` keeps its old
        # 4-tuple shape (name, type, size, modified) for the callers.
        self._tree.insert('', 'end', iid=iid, text=name,
                          image=_tree_icon(icon, _kind_fg(kind)),
                          values=(name, type_label, size_str, ''),
                          tags=(kind,))
        return iid

    def delete(self, first, last=None):
        if first == 0 and (last == 'end' or last is None):
            for iid in self._tree.get_children(''):
                self._tree.delete(iid)
            self._count = 0
        else:
            kids = self._tree.get_children('')
            if last == 'end' or last is None:
                last = len(kids) - 1
            for iid in kids[int(first):int(last) + 1]:
                self._tree.delete(iid)

    # Selection
    def curselection(self):
        return tuple(int(iid) for iid in self._tree.selection())

    def selection_clear(self, first=0, last='end'):
        self._tree.selection_remove(*self._tree.selection())

    def selection_set(self, first, last=None):
        kids = self._tree.get_children('')
        if last is None:
            last = first
        if last == 'end':
            last = len(kids) - 1
        first, last = int(first), int(last)
        for i in range(first, last + 1):
            if 0 <= i < len(kids):
                self._tree.selection_add(kids[i])

    def nearest(self, y):
        iid = self._tree.identify_row(y)
        if iid:
            try:
                return int(iid)
            except (TypeError, ValueError):
                pass
        return -1

    def see(self, index):
        kids = self._tree.get_children('')
        if 0 <= int(index) < len(kids):
            self._tree.see(kids[int(index)])

    # Bind passthrough
    def bind(self, sequence, func, add=None):
        if add is None:
            self._tree.bind(sequence, func)
        else:
            self._tree.bind(sequence, func, add=add)

    def configure(self, **kw):
        # Most listbox-only kwargs (selectmode, activestyle, etc.) are
        # silently ignored. Forward focus/state if asked.
        pass

    def __getattr__(self, name):
        return getattr(self._tree, name)


# ─────────────────────────────────────────────────────────────────────
# Main entry
# ─────────────────────────────────────────────────────────────────────
def build_files_tab(parent, app, embedded=False):
    """Build the exFAT File Manager into `parent`.

    embedded=True is the unified Edit page (exFAT mode): that page owns
    the page header, so this pane starts at the image summary bar.
    Stand-alone hosts get their own page header."""
    parent.configure(bg=COLORS['bg_1'])

    # State — every attribute the rest of the app reads must be set
    app._fm_image_var   = tk.StringVar()
    app._fm_drive       = None
    app._fm_osf         = None
    app._fm_current_dir = None
    app._fm_status_var  = tk.StringVar(value=_('No image mounted'))
    app._fm_path_var    = tk.StringVar(value='—')
    app._fm_entries     = []
    app._fm_used_var    = tk.StringVar(value='')

    # ── Page header (stand-alone hosts only) ──
    if not embedded:
        hdr = PageHeader(
            parent, _('exFAT File Manager'),
            _('Mount an .exfat image and edit its contents in place.'))
        hdr.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

    wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True, padx=px(24), pady=(0, px(20)))

    # ── Mounted-image summary bar ──
    _build_mounted_card(wrap, app, embedded)

    # ── Command bar ──
    _build_toolbar(wrap, app)

    # ── Workspace: listing card + inspector card ──
    body = tk.Frame(wrap, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, pady=(px(12), 0))
    body.columnconfigure(0, weight=1)
    body.columnconfigure(1, weight=0, minsize=px(_INSPECTOR_WIDTH))
    body.rowconfigure(0, weight=1)

    list_col = tk.Frame(body, bg=COLORS['bg_1'])
    list_col.grid(row=0, column=0, sticky='nsew')
    _build_file_table(list_col, app)

    inspector = tk.Frame(body, bg=COLORS['bg_1'])
    inspector.grid(row=0, column=1, sticky='nsew', padx=(px(16), 0))
    _build_inspector(inspector, app)

    _watch_mount_state(app)


def _build_mounted_card(parent, app, embedded=False):
    app._fm_hero = _ImageSummary(
        parent,
        stats=[('Mounted on', 'drive'), ('Image size', 'size'),
               ('Files', 'files'), ('Format', 'format')],
        icon='drive',
        # stand-alone hosts already say this in their page header
        placeholder=(_('Mount an .exfat image and edit its contents in place.')
                     if embedded else ''))
    app._fm_hero.pack(fill='x')
    app._fm_hero.path_prefix = _('Image:') + ' '
    app._fm_hero.set_title(_('No image mounted'), '')
    app._fm_hero.set_stat('format', 'exFAT')
    app._fm_hero.set_badge(_('NO IMAGE'), 'wait')

    # The image path shows on the summary bar's second line; Browse sets
    # it, Mount mounts it (needed before mount, and visible during mount
    # in case you want to switch).
    def _show_image(*_a):
        try:
            app._fm_hero.set_path(app._fm_image_var.get())
        except Exception:
            pass
    app._fm_image_var.trace_add('write', _show_image)

    acts = app._fm_hero.actions
    IconButton(acts, 'folder_open', app._fm_browse, tooltip=_('Browse'),
               variant='secondary').pack(side='left', padx=(0, px(8)))
    app._fm_mount_btn = Button(acts, text=_('Mount'), icon='mount',
                               command=lambda: (_remount_and_refresh(app)))
    app._fm_mount_btn.pack(side='left', padx=(0, px(8)))
    app._fm_remount_btn = IconButton(acts, 'refresh',
                                     lambda: _remount(app),
                                     tooltip=_('Remount'),
                                     variant='secondary')
    app._fm_remount_btn.pack(side='left', padx=(0, px(8)))
    app._fm_dismount_btn = Button(acts, text=_('Dismount'), icon='eject',
                                  command=app._fm_dismount)
    app._fm_dismount_btn.configure(state='disabled')
    app._fm_dismount_btn.pack(side='left')

    # back-compat shims so existing mount/dismount code keeps working
    class _ShimLabel:
        def __init__(self, setter, clear_on_hide=False):
            self._setter = setter
            self._clear = clear_on_hide
        def config(self, **kw):
            if 'text' in kw:
                self._setter(kw['text'])
        configure = config
        def pack(self, *a, **k):
            pass
        def pack_forget(self):
            if self._clear:
                self._setter('')
    app._fm_card_ppsa = _ShimLabel(lambda t: app._fm_hero.id_var.set(t or ''),
                                   clear_on_hide=True)
    app._fm_card_title = _ShimLabel(
        lambda t: app._fm_hero.title_var.set(t or '—'))
    app._fm_card_ver = _ShimLabel(lambda t: None)  # version folded elsewhere
    app._fm_card_path = _ShimLabel(lambda t: app._fm_hero.set_path(t or ''))


def _watch_mount_state(app):
    """Keep the summary bar and the empty-table overlay in step with the
    mount state. The app's mount/dismount code reports through
    _fm_status_var / _fm_path_var; react to those writes (display only —
    mount logic is untouched)."""
    last = {'drive': object()}

    def _sync(*_a):
        drive = getattr(app, '_fm_drive', None)
        try:
            if drive:
                app._fm_empty.place_forget()
            else:
                _place_overlay(app._fm_empty)
        except Exception:
            pass
        if drive != last['drive']:
            last['drive'] = drive
            try:
                app.after_idle(lambda: _refresh_card(app))
            except Exception:
                pass
    app._fm_status_var.trace_add('write', _sync)
    app._fm_path_var.trace_add('write', _sync)
    _sync()


def _remount(app):
    """Remount = dismount then mount the same image."""
    if not app._fm_drive:
        return
    img = app._fm_image_var.get()
    app._fm_dismount()
    if img:
        app.after(500, app._fm_mount)


def _remount_and_refresh(app):
    """Mount and update the hero card afterwards."""
    app._fm_mount()
    app.after(800, lambda: _refresh_card(app))


def _refresh_card(app):
    """Pull mounted-image metadata into the hero card."""
    img = app._fm_image_var.get()
    base = os.path.basename(img) if img else ''
    ppsa = ''
    title = base
    ver = ''
    import re
    m = re.match(r'(PPSA\d{5,})\s+(.*?)\s*(?:\(([\d.]+)\))?\.exfat$',
                 base, re.IGNORECASE)
    if m:
        ppsa = m.group(1)
        title = m.group(2)
        ver = '(' + m.group(3) + ')' if m.group(3) else ''
    if hasattr(app, '_fm_card_ppsa'):
        if ppsa:
            app._fm_card_ppsa.pack(side='left', padx=(0, 8),
                                    before=app._fm_card_title)
            app._fm_card_ppsa.config(text=ppsa)
        else:
            app._fm_card_ppsa.pack_forget()
        app._fm_card_title.config(text=title or _('No image mounted'))
        app._fm_card_ver.config(text=ver)
    # Drive usage (approximate via disk_usage on the mount letter)
    if app._fm_drive:
        try:
            import shutil
            u = shutil.disk_usage(app._fm_drive + '\\')
            app._fm_used_var.set('%s / %s'
                                  % (_format_size(u.used),
                                     _format_size(u.total)))
        except Exception:
            app._fm_used_var.set('')

    # ── hero stats / badge / cover ──
    hero = getattr(app, '_fm_hero', None)
    if hero is not None:
        if app._fm_drive:
            hero.set_stat('drive', app._fm_drive + ':')
            try:
                sz = os.path.getsize(img) if img and os.path.isfile(img) else 0
                hero.set_stat('size', _format_size(sz) if sz else '\u2014')
            except Exception:
                hero.set_stat('size', '\u2014')
            hero.set_badge(_('READY TO EDIT'), 'ready')
            # cover + file count: walk the mounted drive (reads the
            # mounted FS only; does not touch OSFMount/UFS2Tool logic)
            _root = app._fm_drive + ':\\'

            def _walk(root=_root, h=hero):
                n = 0
                cover = None
                try:
                    for dp, _dn, fns in os.walk(root):
                        n += len(fns)
                        if cover is None:
                            for fn in fns:
                                if fn.lower() in ('icon0.png', 'icon0.dds'):
                                    cover = os.path.dirname(
                                        os.path.join(dp, fn))
                                    break
                except Exception:
                    pass

                def _apply(n=n, cover=cover):
                    h.set_stat('files', '{:,}'.format(n))
                    if cover:
                        h.set_cover_from_folder(app, cover)
                try:
                    app.after(0, _apply)
                except Exception:
                    pass
            import threading as _th
            _th.Thread(target=_walk, daemon=True).start()
        else:
            hero.set_stat('drive', '\u2014')
            hero.set_stat('size', '\u2014')
            hero.set_stat('files', '\u2014')
            hero.set_badge(_('NO IMAGE'), 'wait')
            hero.reset_cover()


def _build_toolbar(parent, app):
    """One command bar: Navigation | Creation | Modification, separated
    by hairlines. Commands unchanged."""
    bar = _CommandBar(parent)
    bar.pack(fill='x', pady=(px(16), 0))
    bar.group([
        ('arrow_up', _('Up'), app._fm_go_up),
        ('refresh', _('Refresh'), app._fm_refresh),
    ], rank=3)
    # Backport apply overlays a user-made backport folder onto the
    # mounted image, backing up replaced originals first.
    bar.group([
        ('file_plus', _('Add files'), app._fm_add_file, 0),
        ('folder_in', _('Add folder'), app._fm_add_folder),
        ('folder_plus', _('New folder'), app._fm_new_folder),
        ('backports', _('Apply backport'), lambda: _apply_backport(app), 0),
    ], rank=1)
    bar.group([
        ('replace', _('Replace'), app._fm_replace_file),
        ('trash', _('Delete'), app._fm_delete),
    ], rank=2)
    app._fm_toolbar = bar
    return bar


def _build_file_table(parent, app):
    t = _build_file_table_card(
        parent, [('type', 'Type', 110, 'w'), ('size', 'Size', 100, 'e'),
                 ('mod', 'Modified', 180, 'w')],
        root_icon='drive', path_var=app._fm_path_var)
    t['card'].pack(fill='both', expand=True)
    tree = t['tree']

    app._fm_listbox = ListboxProxy(tree)
    app._fm_tree    = tree

    # Empty state over the table until an image is mounted
    app._fm_empty = DSEmptyState(
        t['body'], icon='drive', title=_('No image mounted'),
        text=_('Mount an .exfat image and edit its contents in place.'),
        bg=COLORS['bg_2'])
    _place_overlay(app._fm_empty)

    # Footer: selection (count + size) left, folder summary / status right
    bg = COLORS['bg_2']
    app._fm_sel_pill = tk.Label(t['foot_left'], text='0 selected',
                                font=FONTS['body'], bg=bg, fg=COLORS['fg_3'])
    app._fm_sel_pill.pack(side='left')
    app._fm_sel_size = tk.Label(t['foot_left'], text='', font=FONTS['code'],
                                bg=bg, fg=COLORS['fg_3'])
    app._fm_sel_size.pack(side='left', padx=(px(8), 0))
    tk.Label(t['foot_right'], textvariable=app._fm_status_var,
             font=FONTS['caption'], bg=bg, fg=COLORS['fg_3']).pack(
                 side='right')
    app._fm_folder_summary = tk.Label(t['foot_right'], text='',
                                      font=FONTS['caption'], bg=bg,
                                      fg=COLORS['fg_3'])
    app._fm_folder_summary.pack(side='right', padx=(0, px(12)))

    tree.bind('<Double-Button-1>', lambda e: app._fm_enter())
    tree.bind('<Button-3>',        app._fm_context_menu)
    tree.bind('<<TreeviewSelect>>', lambda e: _on_tree_select(app))

    def _kb_delete(_=None):
        if app._fm_listbox.curselection() and app._fm_drive:
            app._fm_delete()
        return 'break'
    def _kb_select_all(_=None):
        for iid in tree.get_children(''):
            tree.selection_add(iid)
        return 'break'
    def _kb_enter(_=None):
        if app._fm_listbox.curselection():
            app._fm_enter()
        return 'break'
    def _kb_up(_=None):
        if app._fm_drive:
            app._fm_go_up()
        return 'break'
    def _kb_refresh(_=None):
        if app._fm_drive:
            app._fm_refresh()
        return 'break'

    tree.bind('<Return>',    _kb_enter)
    tree.bind('<KP_Enter>',  _kb_enter)
    tree.bind('<Delete>',    _kb_delete)
    tree.bind('<BackSpace>', _kb_up)
    tree.bind('<Control-a>', _kb_select_all)
    tree.bind('<Control-A>', _kb_select_all)
    tree.bind('<Control-r>', _kb_refresh)
    tree.bind('<F5>',        _kb_refresh)


def _build_inspector(parent, app):
    card, r = _build_inspector_card(parent)
    card.pack(fill='both', expand=True)
    app._fm_insp_icon = r['icon']
    app._fm_insp_name = r['name']
    app._fm_insp_size = r['size']

    d = r['details']
    _inspector_row(d, app, 'Path', '_fm_insp_path_lbl', mono=True)
    _inspector_row(d, app, 'Type', '_fm_insp_type_lbl')
    _inspector_row(d, app, 'Modified', '_fm_insp_mod_lbl')
    _inspector_row(d, app, 'Size', '_fm_insp_bytes_lbl', mono=True)
    _inspector_row(d, app, 'SHA-256', '_fm_insp_hash_lbl', mono=True)

    hex_outer = tk.Frame(r['body'], bg=COLORS['bg_2'])
    hex_outer.pack(fill='x', pady=(px(4), 0))
    tk.Label(hex_outer, text='First 64 bytes', font=FONTS['caption'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w').pack(
                 fill='x', pady=(0, px(4)))
    app._fm_insp_hex = _hex_well(hex_outer)
    app._fm_insp_hex.pack(fill='x')

    # v3.0.0: Replace and Delete live in the toolbar only; Extract stays
    # here — it's the one action the toolbar doesn't offer.
    app._fm_insp_extract = Button(r['actions'], text=_('Extract'),
                                  icon='file_down',
                                  command=lambda: _extract_selected(app))
    app._fm_insp_extract.pack(fill='x')

    _clear_inspector(app)


def _clear_inspector(app):
    if hasattr(app, '_fm_insp_icon'):
        _insp_set_icon(app._fm_insp_icon, None)
        app._fm_insp_name.config(text=_('No selection'), fg=COLORS['fg_3'])
        app._fm_insp_size.config(text='')
        app._fm_insp_path_lbl.config(text='—')
        app._fm_insp_type_lbl.config(text='—')
        app._fm_insp_mod_lbl.config(text='—')
        app._fm_insp_bytes_lbl.config(text='—')
        app._fm_insp_hash_lbl.config(text='—')
        app._fm_insp_hex.config(state='normal')
        app._fm_insp_hex.delete('1.0', 'end')
        app._fm_insp_hex.config(state='disabled')


def _on_tree_select(app):
    sels = app._fm_listbox.curselection()
    if not sels:
        _clear_inspector(app)
        if hasattr(app, '_fm_sel_pill'):
            _style_sel_pill(app._fm_sel_pill, 0)
            app._fm_sel_size.config(text='')
        return

    total_bytes = 0
    for idx in sels:
        if 0 <= idx < len(app._fm_entries):
            total_bytes += app._fm_entries[idx][2] or 0
    _style_sel_pill(app._fm_sel_pill, len(sels))
    app._fm_sel_size.config(text=_format_size(total_bytes)
                                  if total_bytes else '')

    idx = sels[0]
    if not (0 <= idx < len(app._fm_entries)):
        _clear_inspector(app)
        return
    name, is_dir, size = app._fm_entries[idx]
    if name == '..':
        _clear_inspector(app)
        return
    full_path = (os.path.join(app._fm_current_dir, name)
                 if app._fm_current_dir else name)

    kind, type_label = _classify(name, is_dir)
    _insp_set_icon(app._fm_insp_icon, kind)
    app._fm_insp_name.config(text=name, fg=COLORS['fg_0'])
    app._fm_insp_size.config(text=_format_size(size) if size else '')
    app._fm_insp_path_lbl.config(text=_elide_middle(full_path))
    app._fm_insp_type_lbl.config(text=type_label)
    try:
        mtime = os.path.getmtime(full_path)
        app._fm_insp_mod_lbl.config(
            text=time.strftime('%d %b %Y %H:%M', time.localtime(mtime)))
    except Exception:
        app._fm_insp_mod_lbl.config(text='—')
    app._fm_insp_bytes_lbl.config(
        text='%s (%s bytes)' % (_format_size(size), '{:,}'.format(size)) if size else '—')

    app._fm_insp_hex.config(state='normal')
    app._fm_insp_hex.delete('1.0', 'end')
    if is_dir:
        app._fm_insp_hash_lbl.config(text='—  (folder)')
    else:
        try:
            with open(full_path, 'rb') as f:
                data = f.read(64)
            app._fm_insp_hex.insert('end', _hex_lines(data))
        except Exception as e:
            app._fm_insp_hex.insert('end', '(could not read: %s)' % e)
        try:
            import hashlib
            h = hashlib.sha256()
            with open(full_path, 'rb') as f:
                h.update(f.read(1024 * 1024))
            full = h.hexdigest()
            app._fm_insp_hash_lbl.config(text='%s...%s'
                                          % (full[:8], full[-4:]))
        except Exception:
            app._fm_insp_hash_lbl.config(text='—')
    app._fm_insp_hex.config(state='disabled')


def _extract_selected(app):
    sels = app._fm_listbox.curselection()
    if not sels:
        return
    idx = sels[0]
    if not (0 <= idx < len(app._fm_entries)):
        return
    name, is_dir, _size = app._fm_entries[idx]
    if is_dir or name == '..':
        return
    full_path = os.path.join(app._fm_current_dir, name)
    from tkinter import filedialog
    dest = filedialog.asksaveasfilename(
        title='Extract to...', initialfile=name)
    if not dest:
        return
    try:
        import shutil
        shutil.copyfile(full_path, dest)
        app._fm_status_var.set('Extracted: ' + dest)
    except Exception as e:
        app_error(app, 'Extract failed', str(e))


# ─────────────────────────────────────────────────────────────────────
# Apply Backport (v2.0.6) — overlay a user-made backport folder onto
# the currently-mounted exFAT image, backing up every replaced
# original.
#
# Workflow:
#   1) Ask the user to pick the backport SOURCE folder (the folder
#      containing the patched files they've produced — typically the
#      output of Auto Backport or a hand-edited build).
#   2) Walk the source recursively. For every file, compute its target
#      path on the mounted drive, anchored at the current breadcrumb
#      directory (so a user can drop a backport "into" \sce_sys, \,
#      \Media, etc. without rebuilding the folder layout).
#   3) For each target that ALREADY EXISTS as a file on the drive:
#         a. shutil.copy2  -> <backup_dir>\<rel_path>     (preserves mtime)
#         b. shutil.copy2  -> patched file in place
#      For each target that DOES NOT exist:
#         - shutil.copy2   -> patched file (no backup needed)
#      Items that exist on the drive but NOT in the backport folder
#      are PHYSICALLY UNTOUCHED. No deletes are ever issued.
#   4) Zip the backup folder into
#      <image_dir>\<image_stem>.backup-<ts>.zip
#      with a backup_manifest.txt at the root.
#   5) Show a summary dialog with the counts and the path to the zip.
# ─────────────────────────────────────────────────────────────────────
def _apply_backport(app):
    """Toolbar entry-point. Asks what kind of source the user has
    (folder / .ffpkg / .exfat), materialises it to a real folder on
    disk if needed, then runs the overlay on a background thread."""
    import shutil
    import threading
    import time as _time
    from tkinter import filedialog

    # ── Guard: must have a mounted image ──
    if not getattr(app, '_fm_drive', None):
        app_warning(app, _('Not mounted'),
            _('Mount an image first.'))
        return
    if not getattr(app, '_fm_current_dir', None):
        app_warning(app, _('Not mounted'),
            _('Mount an image first.'))
        return

    # ── 1. Ask the user what kind of source they have ──
    kind = _bp_pick_source_kind(app)
    if kind is None:
        return

    # ── 2. Browse for the source path ──
    if kind == 'folder':
        src_path = filedialog.askdirectory(
            title=_('Select backport folder (will overlay onto image)'))
        if not src_path:
            return
        if not os.path.isdir(src_path):
            app_error(app, _('Bad folder'),
                _('Selected path is not a folder.'))
            return
    elif kind == 'ffpkg':
        src_path = filedialog.askopenfilename(
            title=_('Select source .ffpkg'),
            filetypes=[('FFPKG image', '*.ffpkg'),
                       ('All files',   '*.*')])
        if not src_path:
            return
    elif kind == 'exfat':
        src_path = filedialog.askopenfilename(
            title=_('Select source exFAT image'),
            filetypes=[('exFAT image', '*.exfat;*.img'),
                       ('All files',   '*.*')])
        if not src_path:
            return
        # Don't let the user pick the image that's currently mounted.
        img_src_check = _bp_find_image_source_path(app)
        if img_src_check and os.path.abspath(src_path) == \
                os.path.abspath(img_src_check):
            app_error(app, _('Same image'),
                _('Source image is the same file that is currently '
                  'mounted. Pick a different image.'))
            return
    else:
        return

    # ── 3. Materialise source to a real folder on disk ──
    app._fm_status_var.set(_('Preparing backport source...'))
    app.update_idletasks()
    try:
        src_dir, cleanup_fn = _bp_materialize_source(app, kind, src_path)
    except Exception as e:
        app_error(app, _('Source preparation failed'), str(e))
        app._fm_status_var.set(_('Backport cancelled.'))
        return
    if not src_dir:
        app._fm_status_var.set(_('Backport cancelled.'))
        return

    # ── 4. Pre-scan ──
    try:
        items = _bp_scan_source(src_dir)
    except Exception as e:
        try:
            cleanup_fn()
        except Exception:
            pass
        app_error(app, _('Scan failed'), str(e))
        return
    if not items:
        try:
            cleanup_fn()
        except Exception:
            pass
        app_warning(app, _('Empty backport'),
            _('No files were found in the selected source.'))
        return

    # Work out where backups go. We want them next to the SOURCE image
    # file on disk — not on the mounted drive itself, which would
    # nest backups inside the image being edited. Pull the source path
    # from the active build queue entry if we can; otherwise fall back
    # to alongside the mounted drive's root.
    img_src = _bp_find_image_source_path(app)
    if img_src and os.path.isfile(img_src):
        backup_root = os.path.dirname(img_src)
        backup_stem = os.path.splitext(os.path.basename(img_src))[0]
    else:
        # Fallback: drop the backup next to the user's Documents (or
        # cwd). Don't put it on the mounted drive itself.
        backup_root = os.path.expanduser('~')
        backup_stem = 'exfat-image'

    ts          = _time.strftime('%Y%m%d-%H%M%S')
    backup_dir  = os.path.join(backup_root, backup_stem + '.backup-' + ts)
    backup_zip  = backup_dir + '.zip'

    base_dir = app._fm_current_dir
    kind_label = {
        'folder': _('Folder'),
        'ffpkg':  _('.ffpkg image'),
        'exfat':  _('exFAT image'),
    }.get(kind, kind)
    msg = (_('Apply backport to mounted image?') + '\n\n' +
           _('Source kind: ') + kind_label + '\n' +
           _('Source: ') + src_path + '\n' +
           _('Target dir on drive: ') + base_dir + '\n' +
           _('Files in backport: ') + str(len(items)) + '\n\n' +
           _('Originals that get replaced will be backed up to:') + '\n' +
           backup_zip + '\n\n' +
           _('Nothing on the drive is deleted. New files are added; '
             'existing files are replaced (with backup).'))
    if not app_confirm(app, _('Apply backport'), msg):
        try:
            cleanup_fn()
        except Exception:
            pass
        return

    app._fm_status_var.set(
        _('Applying backport (0/%d)...') % len(items))
    app.update_idletasks()

    def _bg():
        stats = {
            'added':     0,
            'replaced':  0,
            'failed':    0,
            'errors':    [],
            'backed_up': [],   # absolute drive paths backed up
        }
        try:
            os.makedirs(backup_dir, exist_ok=True)
        except Exception as e:
            app.after(0, lambda *_a: app_error(app, *_a),
                _('Backup folder failed'),
                _('Could not create backup folder:\n') + str(e))
            app.after(0, app._fm_status_var.set, _('Backport cancelled.'))
            return

        done = 0
        for rel, abs_src in items:
            done += 1
            if done % 10 == 0 or done == len(items):
                app.after(0, app._fm_status_var.set,
                    _('Applying backport (%d/%d)...') % (done, len(items)))

            # Resolve destination path on the drive. Use os.sep so the
            # Windows drive path is correct.
            dst_rel = rel.replace('/', os.sep)
            dst_abs = os.path.join(base_dir, dst_rel)

            existed = os.path.isfile(dst_abs)
            if existed:
                # 1) Back up the original first.
                bkp_path = os.path.join(backup_dir, dst_rel)
                try:
                    os.makedirs(os.path.dirname(bkp_path), exist_ok=True)
                    shutil.copy2(dst_abs, bkp_path)
                    stats['backed_up'].append(dst_abs)
                except Exception as e:
                    stats['failed'] += 1
                    stats['errors'].append(
                        _('backup failed %s: ') % dst_abs + str(e)[:200])
                    continue
                # 2) Replace.
                try:
                    shutil.copy2(abs_src, dst_abs)
                    stats['replaced'] += 1
                except Exception as e:
                    stats['failed'] += 1
                    stats['errors'].append(
                        _('replace failed %s: ') % dst_abs + str(e)[:200])
            else:
                # New file — ensure parent dir, then copy.
                try:
                    os.makedirs(os.path.dirname(dst_abs), exist_ok=True)
                    shutil.copy2(abs_src, dst_abs)
                    stats['added'] += 1
                except Exception as e:
                    stats['failed'] += 1
                    stats['errors'].append(
                        _('add failed %s: ') % dst_abs + str(e)[:200])

        # Write manifest + zip
        zip_ok  = True
        zip_err = ''
        if stats['backed_up']:
            try:
                _bp_write_manifest(backup_dir,
                                    img_src or '<unknown>',
                                    src_dir, base_dir, ts, stats)
                _bp_zip_folder(backup_dir, backup_zip)
            except Exception as e:
                zip_ok  = False
                zip_err = str(e)
        else:
            # Nothing replaced — remove the empty folder so we don't
            # litter the user's disk.
            try:
                os.rmdir(backup_dir)
            except Exception:
                pass

        def _done():
            try:
                app._fm_refresh()
            except Exception:
                pass
            parts = []
            if stats['replaced']: parts.append(_('replaced %d') % stats['replaced'])
            if stats['added']:    parts.append(_('added %d')    % stats['added'])
            if stats['failed']:   parts.append(_('failed %d')   % stats['failed'])
            summary = ', '.join(parts) if parts else _('no changes')
            app._fm_status_var.set(_('Backport applied: ') + summary)

            lines = [_('Backport applied.'), '']
            lines.append(_('Replaced: ') + str(stats['replaced']))
            lines.append(_('Added: ')    + str(stats['added']))
            if stats['failed']:
                lines.append(_('Failed: ') + str(stats['failed']))
            lines.append('')
            if stats['backed_up']:
                if zip_ok:
                    lines.append(_('Backup of replaced originals:'))
                    lines.append(backup_zip)
                else:
                    lines.append(_('Backup folder kept (zip failed):'))
                    lines.append(backup_dir)
                    lines.append(_('Zip error: ') + zip_err)
            else:
                lines.append(_('No originals were replaced, '
                               'so no backup was needed.'))
            if stats['errors']:
                lines.append('')
                lines.append(_('First errors:'))
                for e in stats['errors'][:5]:
                    lines.append('  - ' + e)
                if len(stats['errors']) > 5:
                    lines.append('  ... (%d more)' % (len(stats['errors']) - 5))
            if stats['failed']:
                app_warning(app, _('Backport finished with errors'),
                    '\n'.join(lines))
            else:
                app_success(app, _('Backport applied'),
                    '\n'.join(lines))
            # Clean up any temp dir that held the materialised source.
            try:
                cleanup_fn()
            except Exception:
                pass

        app.after(0, _done)

    threading.Thread(target=_bg, daemon=True,
                     name='exfat-apply-backport').start()


def _bp_scan_source(src_dir):
    """Walk `src_dir` recursively, returning a sorted list of
    (relative_path_with_forward_slashes, absolute_source_path) tuples."""
    items = []
    src_dir = os.path.abspath(src_dir)
    for root, _dirs, files in os.walk(src_dir):
        for f in files:
            ap = os.path.join(root, f)
            try:
                rel = os.path.relpath(ap, src_dir).replace(os.sep, '/')
            except Exception:
                continue
            if not rel or rel.startswith('..'):
                continue
            items.append((rel, ap))
    items.sort(key=lambda t: t[0].lower())
    return items


def _bp_find_image_source_path(app):
    """Try to find the on-disk path of the image currently mounted in
    the Edit exFAT tab. The app may keep this in a few different places
    depending on whether the mount was triggered from the build queue
    or from the manual Browse → Mount flow."""
    # Direct attribute set by the manual mount flow.
    for attr in ('_fm_mounted_image_path',
                 '_fm_image_path',
                 '_fm_source_path',
                 '_fm_image_var'):
        v = getattr(app, attr, None)
        if v is None:
            continue
        try:
            # StringVar?
            v = v.get() if hasattr(v, 'get') else v
        except Exception:
            continue
        if isinstance(v, str) and v and os.path.isfile(v):
            return v
    # Build queue: active item's output path.
    try:
        idx = getattr(app, '_queue_active_index', None)
        if idx is not None and 0 <= idx < len(app._queue):
            item = app._queue[idx]
            for key in ('out_path', 'output_path', 'image_path'):
                p = item.get(key) if isinstance(item, dict) else None
                if p and os.path.isfile(p):
                    return p
    except Exception:
        pass
    return None


def _bp_write_manifest(backup_dir, img, src_dir, base_dir, ts, stats):
    """Write a backup_manifest.txt at backup_dir/ root with enough
    info to restore the originals if needed."""
    lines = []
    lines.append('exFAT Image Builder \u2014 backport backup manifest')
    lines.append('=' * 50)
    lines.append('Timestamp:        ' + ts)
    lines.append('Image:            ' + img)
    lines.append('Backport source:  ' + src_dir)
    lines.append('Anchored at:      ' + base_dir)
    lines.append('Replaced count:   ' + str(stats['replaced']))
    lines.append('Added count:      ' + str(stats['added']))
    lines.append('Failed count:     ' + str(stats['failed']))
    lines.append('')
    lines.append('Files backed up (originals from the mounted image, '
                 'copied BEFORE replace):')
    for p in stats['backed_up']:
        lines.append('  ' + p)
    if stats['errors']:
        lines.append('')
        lines.append('Errors:')
        for e in stats['errors']:
            lines.append('  - ' + e)
    lines.append('')
    lines.append('To restore: mount the same image in Edit exFAT, '
                 'navigate to the same anchor folder, and use '
                 'Add folder on the extracted backup folder. Replaced '
                 'files will be put back in place (overwrite the patched '
                 'copies).')
    try:
        with open(os.path.join(backup_dir, 'backup_manifest.txt'),
                  'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines))
    except Exception:
        pass


def _bp_zip_folder(backup_dir, backup_zip):
    """Zip the backup folder. Removes the folder on success so the
    zip is the canonical artifact."""
    import zipfile as _zf
    import shutil as _sh
    with _zf.ZipFile(backup_zip, 'w', _zf.ZIP_DEFLATED) as zf:
        for root, _dirs, files in os.walk(backup_dir):
            for f in files:
                ap  = os.path.join(root, f)
                rel = os.path.relpath(ap, backup_dir)
                zf.write(ap, rel)
    try:
        _sh.rmtree(backup_dir, ignore_errors=True)
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────
# Source picker — Folder / .ffpkg / .exfat
# ─────────────────────────────────────────────────────────────────────
def _bp_pick_source_kind(app):
    """Show a small modal dialog asking the user what kind of backport
    source they have. Returns 'folder', 'ffpkg', 'exfat', or None if
    the user cancels.
    """
    result = {'kind': None}
    dlg, body, footer = _dialog_shell(
        app, _('Apply backport \u2014 pick source'),
        _('What\u2019s your backport source?'),
        _('Pick the format that matches what you have.'), width=500)

    def _pick(k):
        result['kind'] = k
        dlg.destroy()

    rows = tk.Frame(body, bg=COLORS['bg_2'])
    rows.pack(fill='x', pady=(px(16), 0))
    _choice_row(rows, 'folder', _('A folder'),
         _('A backport folder like the ones produced by Auto Backport '
           '(e.g. fakelib/, sce_module/, sce_sys/, eboot.bin).'),
         lambda: _pick('folder'), body.wrap)
    _choice_row(rows, 'package', _('A .ffpkg image'),
         _('Read the backport tree out of a .ffpkg using UFS2Tool, '
           'then overlay it onto the mounted image.'),
         lambda: _pick('ffpkg'), body.wrap)
    _choice_row(rows, 'drive', _('An exFAT image'),
         _('Mount a .exfat / .img read-only via OSFMount, copy its '
           'contents to a temp folder, then overlay.'),
         lambda: _pick('exfat'), body.wrap)

    _dialog_button(footer, _('Cancel'), dlg.destroy)
    dlg.bind('<Escape>', lambda e: dlg.destroy())
    _dialog_place(dlg, app)
    app.wait_window(dlg)
    return result['kind']


# ─────────────────────────────────────────────────────────────────────
# Source materialisation (Folder / .ffpkg / .exfat)
# ─────────────────────────────────────────────────────────────────────
def _bp_materialize_source(app, kind, src_path):
    """Turn the chosen source into a real folder on disk.
    Returns (src_dir, cleanup_fn). cleanup_fn is always callable."""
    if kind == 'folder':
        return src_path, (lambda: None)
    if kind == 'ffpkg':
        return _bp_ffpkg_to_temp(app, src_path)
    if kind == 'exfat':
        return _bp_exfat_to_temp(app, src_path)
    raise ValueError('unknown kind: ' + repr(kind))


def _bp_ffpkg_to_temp(app, ffpkg_path):
    """Extract the whole tree from `ffpkg_path` into a temp dir using
    UFS2Tool. Returns (temp_dir, cleanup_fn).
    """
    import shutil as _sh
    if not os.path.isfile(ffpkg_path):
        raise RuntimeError(_('Source .ffpkg does not exist: ') + ffpkg_path)

    # extract_ufs2tool and tempfile come in via `from exfat_builder import *`
    exe = extract_ufs2tool()

    tmp = tempfile.mkdtemp(prefix='exfat_bp_ffpkg_src_')
    def _cleanup():
        try:
            _sh.rmtree(tmp, ignore_errors=True)
        except Exception:
            pass

    try:
        stats = {'files': 0, 'errors': []}
        _bp_ffpkg_walk(exe, ffpkg_path, '/', tmp, stats)
        if not stats['files']:
            raise RuntimeError(
                _('No files were extracted from the source .ffpkg.'))
        return tmp, _cleanup
    except Exception:
        _cleanup()
        raise


def _bp_run_ufs2(exe, args, timeout=120):
    """Tiny wrapper around UFS2Tool.exe; returns (returncode, combined_text).
    Used only by the apply-backport ffpkg-source path."""
    try:
        result = subprocess.run(
            [exe] + list(args),
            capture_output=True, text=True,
            encoding='utf-8', errors='replace',
            timeout=timeout, creationflags=_NO_WIN_FLAGS)
        out = (result.stdout or '') + (result.stderr or '')
        return result.returncode, out
    except subprocess.TimeoutExpired:
        return -1, 'UFS2Tool timeout'
    except Exception as e:
        return -1, 'UFS2Tool error: ' + str(e)


def _bp_parse_ls(text):
    """Parse UFS2Tool `ls` output. Returns a list of (name, is_dir, size).
    Tolerant of formatting variations: matches the leading 'd' for
    directories and falls back to a heuristic for unrecognised lines."""
    entries = []
    for raw in (text or '').splitlines():
        line = raw.rstrip()
        if not line:
            continue
        # Skip headers / summary lines
        low = line.lower()
        if 'total' in low and ':' not in line:
            continue
        # UFS2Tool typical format: "drwxr-xr-x   N   <size>   name"
        # or "-rw-r--r--   N   <size>   name". We split on whitespace
        # and pick the last token as the name (UFS2Tool quotes names
        # containing spaces; if not quoted, names with spaces won't
        # parse cleanly here — same limitation as the existing tab.
        parts = line.split()
        if not parts:
            continue
        first = parts[0]
        is_dir = first.startswith('d') or first.lower() == 'directory'
        # Try to get a size — the third field on a typical ls line.
        size = 0
        if len(parts) >= 3:
            try:
                size = int(parts[2])
            except Exception:
                size = 0
        # Name is the LAST token (best-effort).
        name = parts[-1]
        # Skip dot entries.
        if name in ('.', '..'):
            continue
        entries.append((name, is_dir, size))
    return entries


def _bp_join_fs(*parts):
    """Join UFS2 fs path components with forward slashes, collapsing
    duplicate separators. Strips before filtering so that '/' as a
    base segment doesn't leave an empty string in the join."""
    stripped = [p.strip('/') for p in parts if p]
    parts2   = [s for s in stripped if s]
    out      = '/'.join(parts2)
    return '/' + out if out else '/'


def _bp_ffpkg_walk(exe, img, fs_dir, local_dir, stats, _depth=0):
    """Recursive helper. Walks the fs tree and extracts every regular
    file under `local_dir`, preserving the path."""
    if _depth > 32:
        stats['errors'].append(_('recursion too deep at ') + fs_dir)
        return
    rc, out = _bp_run_ufs2(exe, ['ls', img, fs_dir], timeout=60)
    if rc != 0:
        stats['errors'].append('ls %s: %s' % (fs_dir, out.strip()[:200]))
        return
    parsed = _bp_parse_ls(out)
    for name, is_dir, _sz in parsed:
        child_fs = _bp_join_fs(fs_dir, name)
        if is_dir:
            _bp_ffpkg_walk(exe, img, child_fs, local_dir, stats, _depth + 1)
        else:
            rc, out = _bp_run_ufs2(exe, ['extract', img, local_dir, child_fs],
                                    timeout=300)
            if rc != 0:
                stats['errors'].append('extract %s: %s'
                                        % (child_fs, out.strip()[:200]))
                continue
            stats['files'] += 1


def _bp_exfat_to_temp(app, img_path):
    """Mount `img_path` read-only via OSFMount, copy its tree into a
    temp dir, then dismount. Returns (temp_dir, cleanup_fn)."""
    import shutil as _sh
    import ctypes as _ct
    import time as _t2
    if not os.path.isfile(img_path):
        raise RuntimeError(_('Source exFAT image does not exist: ') + img_path)

    try:
        osf = app._find_osfmount()
    except Exception:
        osf = None
    if not osf:
        raise RuntimeError(_(
            'OSFMount is required to read exFAT images but was not '
            'found. Install it (or set its path in Settings) and try '
            'again.'))

    tmp = tempfile.mkdtemp(prefix='exfat_bp_exfat_src_')
    mounted = {'drive': None}

    def _cleanup():
        if mounted['drive']:
            try:
                subprocess.run([osf, '-d', '-m', mounted['drive']],
                               capture_output=True, timeout=30,
                               creationflags=_NO_WIN_FLAGS)
            except Exception:
                pass
        try:
            _sh.rmtree(tmp, ignore_errors=True)
        except Exception:
            pass

    try:
        drives_bitmask = used_letter_mask()
        free_letter = None
        for i in range(25, 3, -1):
            if not (drives_bitmask & (1 << i)):
                free_letter = chr(65 + i) + ':'
                break
        if not free_letter:
            raise RuntimeError(_('No free drive letters available for mount.'))

        result = subprocess.run(
            [osf, '-a', '-t', 'file', '-f', img_path,
             '-m', free_letter, '-o', 'ro,rem'],
            capture_output=True, text=True, timeout=120,
            creationflags=_NO_WIN_FLAGS)
        if result.returncode != 0:
            raise RuntimeError(_('OSFMount mount failed: ') +
                (result.stderr or result.stdout or '')[:300])
        mounted['drive'] = free_letter

        # (loop variable must not be called "_": that would shadow the
        # translation function for this whole function and turn every
        # error message here into an UnboundLocalError)
        for _attempt in range(30):
            if os.path.exists(free_letter + '\\'):
                break
            _t2.sleep(0.5)
        else:
            raise RuntimeError(_('Mounted drive did not appear in time.'))

        # Copy tree using shutil.
        import shutil as _sh2
        for root, _dirs, files in os.walk(free_letter + '\\'):
            try:
                rel_root = os.path.relpath(root, free_letter + '\\')
            except Exception:
                continue
            if rel_root == '.':
                rel_root = ''
            for fn in files:
                src = os.path.join(root, fn)
                dst = os.path.join(tmp, rel_root, fn) if rel_root \
                                                     else os.path.join(tmp, fn)
                try:
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    _sh2.copy2(src, dst)
                except Exception:
                    pass

        # Eager dismount.
        try:
            subprocess.run([osf, '-d', '-m', mounted['drive']],
                           capture_output=True, timeout=30,
                           creationflags=_NO_WIN_FLAGS)
        except Exception:
            pass
        mounted['drive'] = None

        return tmp, _cleanup
    except Exception:
        _cleanup()
        raise
