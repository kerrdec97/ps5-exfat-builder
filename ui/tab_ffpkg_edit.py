"""
ui/tab_ffpkg_edit.py — Edit ffpkg sub-tab (v2.9.0, lean version).

Mirrors the visual layout of Edit exFAT but drives UFS2Tool CLI verbs
instead of mounting via OSFMount. Each operation calls UFS2Tool.exe
once and refreshes the listing.

Supported operations (per UFS2Tool v2.5+):
  - ls       — list a directory inside the image
  - add      — add a file or directory (recursive)
  - delete   — delete a file or directory (recursive)
  - replace  — replace a file
  - rename   — rename a file or directory
  - stat     — read inode info (size, perms, timestamps)
  - extract  — extract a file (used for on-demand hex preview)

NOT supported by UFS2Tool's CLI:
  - mkdir    — workaround: add a .keep placeholder file inside the
               new path, which creates the directory as a side effect.
"""

import os
import sys
import time
import tempfile
import subprocess
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

from tkinter_theme import COLORS, FONTS
from ui.shared.empty_state import EmptyState

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings, extract_ufs2tool, _NO_WIN_FLAGS


# ─────────────────────────────────────────────────────────────────────
# Constants
# ─────────────────────────────────────────────────────────────────────
_INSPECTOR_WIDTH = 288


def _style_sel_pill(pill, n):
    """Selection-count pill: muted at zero, accent when items are
    selected. Presentation only."""
    if n:
        pill.config(text='%d selected' % n,
                    bg=COLORS['accent_15'], fg=COLORS['accent_hi'])
    else:
        pill.config(text='0 selected',
                    bg=COLORS['bg_3'], fg=COLORS['fg_4'])

_TYPE_ICONS = {
    'folder':  '\U0001f4c1',  # 📁
    'exec':    '\u2b22',       # ⬢
    'log':     '\u2261',       # ≡
    'text':    '\U0001f4c4',  # 📄
    'data':    '\u25c6',       # ◆
    'config':  '\u2699',       # ⚙
    'binary':  '\u26ac',       # ⚬
    'unknown': '\U0001f4be',  # 💾
}
_TYPE_COLORS = {
    'folder':  COLORS['accent'],
    'exec':    COLORS['warn'],
    'log':     COLORS['success_hi'],
    'text':    COLORS['fg_3'],
    'data':    COLORS['purple'],
    'config':  COLORS['fg_3'],
    'binary':  COLORS['fg_3'],
    'unknown': COLORS['fg_4'],
}


def _classify(name, is_dir):
    """Classify a directory entry by name + known type.

    is_dir can be True, False, or None (unknown). When None, we apply
    a "looks like folder" heuristic: names with no extension that don't
    match a known file pattern are treated as folders.
    """
    if is_dir is True:
        return ('folder', 'Folder')
    lname = name.lower()
    if is_dir is False:
        # Confirmed file — skip the folder heuristic
        pass
    else:
        # is_dir is None — apply heuristic.
        # No dot in name + not a known file-like name => likely folder.
        if '.' not in lname and lname not in (
                'eboot', 'sidbase', 'keystone', 'param', 'pic0', 'pic1',
                'icon0'):
            return ('folder', 'Folder')
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
    if lname.endswith(('.bin', '.dat', '.pkg')):
        return ('binary', 'Binary')
    return ('unknown',
            name.split('.')[-1].upper() if '.' in name else 'File')


def _fmt_size(b):
    if not b:
        return '0 B'
    for unit, div in (('GB', 1024**3), ('MB', 1024**2), ('KB', 1024)):
        if b >= div:
            if div >= 1024**2:
                return '%.2f %s' % (b / div, unit)
            return '%d %s' % (b // div, unit)
    return '%d B' % b


# ─────────────────────────────────────────────────────────────────────
# UFS2Tool CLI helpers
# ─────────────────────────────────────────────────────────────────────
def _ufs2_exe(app):
    """Ensure UFS2Tool.exe is extracted and return its path."""
    if not getattr(app, '_ffpkg_ufs2_exe', None) or not os.path.isfile(app._ffpkg_ufs2_exe):
        app._ffpkg_ufs2_exe = extract_ufs2tool(
            app._settings.get('temp_dir') or None)
    return app._ffpkg_ufs2_exe


def _run_ufs2(app, args, timeout=120):
    """Run UFS2Tool.exe with the given args and return (rc, stdout)."""
    exe = _ufs2_exe(app)
    enc = 'mbcs' if sys.platform.startswith('win') else 'utf-8'
    try:
        proc = subprocess.run(
            [exe] + list(args),
            capture_output=True, text=True,
            encoding=enc, errors='replace',
            timeout=timeout,
            creationflags=_NO_WIN_FLAGS)
        out = (proc.stdout or '') + (proc.stderr or '')
        return proc.returncode, out
    except subprocess.TimeoutExpired:
        return -1, '(timeout after %ds)' % timeout
    except Exception as e:
        return -2, '(' + str(e) + ')'


def _parse_ls(text):
    """Parse `UFS2Tool.exe ls` output into a list of (name, is_dir, size).

    UFS2Tool v2.5+ output format (confirmed from real binary):

        Directory '/' (31 entries):
          DIR   inode=     2  .
          DIR   inode=  4441  assets
          FILE  inode=  5232  copytexture2d_p.ags
          FILE  inode=  5256  eboot.bin

    Each entry line: `  <TYPE>  inode= <inode-number>  <name>` where
    <TYPE> is either DIR or FILE. UFS2Tool does not currently report
    file sizes in `ls`, so size is always 0 — we'd need a separate
    `stat` call to fill it in.

    Older / alternate formats are kept as fallback heuristics in case
    UFS2Tool changes its output again.
    """
    import re as _re
    seen = set()
    entries = []
    # Primary pattern: '  DIR   inode=     2  .' or '  FILE  inode=  5256  eboot.bin'
    primary = _re.compile(
        r'^\s*(DIR|FILE)\s+inode=\s*(\d+)\s+(.+?)\s*$')

    for raw in text.splitlines():
        line = raw.rstrip()
        if not line:
            continue
        stripped = line.strip()
        low = stripped.lower()

        # Skip header lines
        if (low.startswith('total ') or low.startswith('listing ')
                or low.startswith('directory ') or low.startswith('contents of')
                or low.startswith('---') or low.startswith('===')
                or low == 'name'
                or (low.startswith('name ') and 'size' in low)):
            continue
        # Quoted-path header: "'/' (31 entries):"
        if ((stripped.startswith("'") or stripped.startswith('"'))
                and stripped.endswith(':')):
            continue

        # ─── Primary: UFS2Tool v2.5+ format ───
        m = primary.match(line)
        if m:
            kind, _inode_num, name = m.group(1), m.group(2), m.group(3).strip()
            if not name or name in ('.', '..'):
                continue
            if name in seen:
                continue
            seen.add(name)
            entries.append((name, kind == 'DIR', 0))
            continue

        # ─── Fallbacks for alternate / older formats ───
        is_dir = None
        size = 0
        name = None
        parts = stripped.split()
        if not parts:
            continue

        # ls -l style
        if (len(parts[0]) >= 10 and parts[0][0] in 'd-l'
                and all(c in 'rwx-st' for c in parts[0][1:10])):
            is_dir = parts[0][0] == 'd'
            name = parts[-1]
            for col in parts[1:-1]:
                try:
                    v = int(col)
                    if v > size:
                        size = v
                except ValueError:
                    continue
        # Explicit kind prefix on its own
        elif parts[0].lower() in ('directory', 'dir', 'folder'):
            is_dir = True
            name = ' '.join(parts[1:])
        elif parts[0].lower() == 'file' and len(parts) >= 2:
            is_dir = False
            name = ' '.join(parts[1:])
        elif parts[0] in ('[DIR]', '<DIR>'):
            is_dir = True
            name = ' '.join(parts[1:]) if len(parts) > 1 else ''
        # Trailing-slash dir marker
        elif stripped.endswith('/') or stripped.endswith('\\'):
            is_dir = True
            name = stripped.rstrip('/\\')
        else:
            # Unknown format — last token as name
            name = parts[-1]
            is_dir = None

        if not name or name in ('.', '..'):
            continue
        if name.lower() == 'inode' or name.lower().startswith('inode='):
            continue
        if name in seen:
            continue
        seen.add(name)
        entries.append((name, is_dir, size))

    # Sort: dirs first, then unknowns, then files; all alphabetical
    def _sort_key(e):
        _name, _isd, _ = e
        rank = 0 if _isd is True else (1 if _isd is None else 2)
        return (rank, _name.lower())
    entries.sort(key=_sort_key)
    return entries


def _join_fs_path(*parts):
    """Join filesystem paths inside the image using forward slashes.

    Bug fix (v2.0.6): when one of the parts is exactly '/', the old
    impl produced double-slashed output like '//eboot.bin' because
    '/'.strip('/') == '' and the empty result was still joined. Now
    we strip first, then filter empties.
    """
    stripped = [p.strip('/') for p in parts if p]
    parts2   = [s for s in stripped if s]
    out      = '/'.join(parts2)
    return '/' + out if out else '/'


def _basename_fs(path):
    p = path.rstrip('/')
    if not p or p == '/':
        return '/'
    return p.rsplit('/', 1)[-1]


def _dirname_fs(path):
    p = path.rstrip('/')
    if not p or p == '/':
        return '/'
    return p.rsplit('/', 1)[0] or '/'


# ─────────────────────────────────────────────────────────────────────
# Main entry
# ─────────────────────────────────────────────────────────────────────
def build_ffpkg_edit_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    # State
    app._fe_image_var   = tk.StringVar()
    app._fe_current     = '/'              # current fs-path inside the image
    app._fe_entries     = []               # list of (name, is_dir, size)
    app._fe_ls_cache    = {}               # path -> entries list
    app._fe_status_var  = tk.StringVar(value=_('No image loaded'))
    app._fe_path_var    = tk.StringVar(value='/')

    # ── Page head ──
    head = tk.Frame(parent, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 6))
    icon_tile = tk.Frame(head, bg=COLORS['accent_15'], width=32, height=32)
    icon_tile.pack(side='left')
    icon_tile.pack_propagate(False)
    tk.Label(icon_tile, text='\u270f',
             bg=COLORS['accent_15'], fg=COLORS['accent'],
             font=('Segoe UI', 14)).pack(expand=True)
    title_col = tk.Frame(head, bg=COLORS['bg_1'])
    title_col.pack(side='left', padx=(10, 0))
    tk.Label(title_col, text=_('Edit ffpkg Image'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(anchor='w')
    tk.Label(title_col,
             text=_('Add, replace, rename, or delete files inside a .ffpkg via UFS2Tool.'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_5']
             ).pack(anchor='w')

    # ── File picker hero card ──
    _build_image_card(parent, app)

    # ── Body: split layout ──
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, padx=24, pady=(8, 12))
    body.columnconfigure(0, weight=1)
    body.columnconfigure(1, weight=0)
    body.rowconfigure(0, weight=1)

    list_col = tk.Frame(body, bg=COLORS['bg_1'])
    list_col.grid(row=0, column=0, sticky='nsew')
    _build_toolbar(list_col, app)
    _build_table(list_col, app)

    inspector = tk.Frame(body, bg=COLORS['bg_2'],
                         width=_INSPECTOR_WIDTH,
                         highlightbackground=COLORS['border_2'],
                         highlightthickness=1)
    inspector.grid(row=0, column=1, sticky='nsew', padx=(12, 0))
    inspector.grid_propagate(False)
    _build_inspector(inspector, app)


# ─────────────────────────────────────────────────────────────────────
# Hero card — file picker for the .ffpkg
# ─────────────────────────────────────────────────────────────────────
def _build_image_card(parent, app):
    from ui.shared.hero import GameHero
    wrap = tk.Frame(parent, bg=COLORS['bg_1'])
    wrap.pack(fill='x', padx=24, pady=(0, 8))

    app._fe_hero = GameHero(
        wrap,
        stats=[('Image Size', 'size'), ('Files', 'files'),
               ('Format', 'format')],
        cover_glyph='\U0001f4e6', cover_size=110)
    app._fe_hero.pack(side='left', fill='x', expand=True)
    app._fe_hero.set_title(_('No image loaded'), '')
    app._fe_hero.set_stat('format', 'ffpkg')
    app._fe_hero.set_badge(_('NO IMAGE'), 'wait')

    # Buttons column
    btns = tk.Frame(wrap, bg=COLORS['bg_1'])
    btns.pack(side='right', padx=(12, 0))
    _accent_btn(btns, _('Open Image...'),
        command=lambda: _browse_image(app)).pack(side='top', pady=(0, 4))
    app._fe_close_btn = _danger_btn(btns, '\u23cf  ' + _('Close'),
        command=lambda: _close_image(app))
    app._fe_close_btn.configure(state='disabled')
    app._fe_close_btn.pack(side='top')

    # back-compat shims: existing open/close code sets these "labels";
    # route their text into the hero so logic stays untouched.
    class _ShimLabel:
        def __init__(self, setter):
            self._setter = setter
        def config(self, **kw):
            if 'text' in kw:
                self._setter(kw['text'])
        configure = config
        def pack(self, *a, **k):
            pass
        def pack_forget(self):
            pass
    app._fe_card_ppsa = _ShimLabel(lambda t: app._fe_hero.id_var.set(t or ''))
    app._fe_card_title = _ShimLabel(
        lambda t: app._fe_hero.title_var.set((t or '').upper()))
    app._fe_card_size = _ShimLabel(
        lambda t: app._fe_hero.set_stat('size', t or '\u2014'))
    app._fe_card_path = _ShimLabel(lambda t: app._fe_hero.set_path(t or ''))


def _browse_image(app):
    p = filedialog.askopenfilename(
        title=_('Select .ffpkg image'),
        filetypes=[('ffpkg images', '*.ffpkg'),
                   ('All files',    '*.*')])
    if not p:
        return
    open_ffpkg_path(app, p.replace('/', '\\'))


def open_ffpkg_path(app, p):
    """Open a .ffpkg by path. Shared by the Browse button and the unified
    Edit-tab opener. Behaviour is identical to the former _browse_image
    post-dialog steps."""
    app._fe_image_var.set(p)
    # Try to parse PPSA + title from filename
    import re
    base = os.path.basename(p)
    m = re.match(r'(PPSA\d{5,})\s+(.*?)\s*(?:\(([\d.]+)\))?\.ffpkg$',
                 base, re.IGNORECASE)
    if m:
        app._fe_card_ppsa.pack(side='left', padx=(0, 8),
                                before=app._fe_card_title)
        app._fe_card_ppsa.config(text=m.group(1))
        app._fe_card_title.config(text=m.group(2))
    else:
        app._fe_card_ppsa.pack_forget()
        app._fe_card_title.config(
            text=os.path.splitext(base)[0])
    app._fe_close_btn.configure(state='normal')
    try:
        sz = os.path.getsize(p)
        app._fe_card_size.config(text=_fmt_size(sz))
        app._fe_card_size.pack(side='left', padx=(8, 0))
    except Exception:
        app._fe_card_size.config(text='')
        app._fe_card_size.pack_forget()
    try:
        app._fe_empty.place_forget()
    except Exception:
        pass
    app._fe_current = '/'
    app._fe_ls_cache.clear()
    _refresh(app)
    try:
        app._fe_hero.set_badge(_('READY TO EDIT'), 'ready')
    except Exception:
        pass


def _close_image(app):
    app._fe_image_var.set('')
    app._fe_card_title.config(text=_('No image loaded'))
    app._fe_card_ppsa.pack_forget()
    try:
        app._fe_card_size.config(text='')
        app._fe_card_size.pack_forget()
        app._fe_empty.place(relx=0, rely=0, relwidth=1, relheight=1)
    except Exception:
        pass
    app._fe_close_btn.configure(state='disabled')
    app._fe_listbox.delete(0, 'end')
    app._fe_entries = []
    app._fe_ls_cache.clear()
    app._fe_current = '/'
    app._fe_path_var.set('/')
    app._fe_status_var.set(_('No image loaded'))
    try:
        app._fe_hero.set_badge(_('NO IMAGE'), 'wait')
        app._fe_hero.set_stat('files', '\u2014')
        app._fe_hero.reset_cover()
    except Exception:
        pass
    _clear_inspector(app)


# ─────────────────────────────────────────────────────────────────────
# Toolbar
# ─────────────────────────────────────────────────────────────────────
def _build_toolbar(parent, app):
    tb = tk.Frame(parent, bg=COLORS['bg_1'])
    tb.pack(fill='x', pady=(0, 6))

    # Breadcrumb
    bc = tk.Frame(tb, bg=COLORS['bg_1'])
    bc.pack(side='left')
    tk.Label(bc, text='\U0001f4e6',
             font=('Segoe UI', 10), bg=COLORS['bg_1'],
             fg=COLORS['accent']).pack(side='left')
    tk.Label(bc, text=' \u203a ',
             font=FONTS['mono_sm'], bg=COLORS['bg_1'],
             fg=COLORS['fg_6']).pack(side='left')
    tk.Label(bc, textvariable=app._fe_path_var,
             font=FONTS['mono_sm'], bg=COLORS['bg_1'],
             fg=COLORS['fg_3']).pack(side='left')

    # Button group — labeled sections (Navigation / Creation /
    # Modification / Danger). Layout only; commands unchanged.
    btns = tk.Frame(tb, bg=COLORS['bg_1'])
    btns.pack(side='right')

    def _group(title):
        col = tk.Frame(btns, bg=COLORS['bg_1'])
        col.pack(side='left', padx=(0, 10))
        tk.Label(col, text=title.upper(),
                 font=(FONTS['mono_sm'][0], 7, 'bold'), bg=COLORS['bg_1'],
                 fg=COLORS['fg_5']).pack(anchor='w', pady=(0, 2))
        row = tk.Frame(col, bg=COLORS['bg_1'])
        row.pack(anchor='w')
        return row

    def _sep():
        tk.Frame(btns, bg=COLORS['border_3'], width=1
                 ).pack(side='left', fill='y', padx=(0, 10), pady=(12, 2))

    def _tb(parent, text, cmd, kind='ghost'):
        return _icon_btn(parent, text, cmd, kind=kind)

    g_nav = _group(_('Navigation'))
    _tb(g_nav, '\u2191  ' + _('Up'), lambda: _go_up(app)
        ).pack(side='left', padx=(0, 4))
    _tb(g_nav, '\u21bb  ' + _('Refresh'), lambda: _refresh(app)
        ).pack(side='left')
    _sep()

    g_new = _group(_('Creation'))
    _tb(g_new, '\u2795  ' + _('Add files'), lambda: _add_files(app),
        kind='accent').pack(side='left', padx=(0, 4))
    _tb(g_new, '\U0001f4c1  ' + _('Add folder'), lambda: _add_folder(app)
        ).pack(side='left', padx=(0, 4))
    _tb(g_new, '\U0001f4c2  ' + _('New folder'), lambda: _new_folder(app)
        ).pack(side='left', padx=(0, 4))
    _tb(g_new, '\U0001f4e5  ' + _('Apply backport'),
        lambda: _apply_backport(app), kind='accent').pack(side='left',
                                                          padx=(0, 4))
    _tb(g_new, '\U0001f50d  ' + _('Compare'), lambda: _compare_ffpkgs(app)
        ).pack(side='left')
    _sep()

    g_mod = _group(_('Modification'))
    _tb(g_mod, '\u21bb  ' + _('Replace'), lambda: _replace_file(app)
        ).pack(side='left', padx=(0, 4))
    _tb(g_mod, '\u270f  ' + _('Rename'), lambda: _rename(app)
        ).pack(side='left')
    _sep()

    g_dng = _group(_('Danger'))
    _tb(g_dng, '\U0001f5d1  ' + _('Delete'), lambda: _delete(app),
        kind='danger').pack(side='left')


# ─────────────────────────────────────────────────────────────────────
# File table (Treeview)
# ─────────────────────────────────────────────────────────────────────
def _build_table(parent, app):
    outer = tk.Frame(parent, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_3'],
                     highlightthickness=1)
    outer.pack(fill='both', expand=True)

    style = ttk.Style()
    style.configure('FfpkgEdit.Treeview',
        background=COLORS['bg_2'], foreground=COLORS['fg_1'],
        fieldbackground=COLORS['bg_2'],
        bordercolor=COLORS['border_3'],
        lightcolor=COLORS['bg_2'], darkcolor=COLORS['bg_2'],
        rowheight=22,
        font=(FONTS['mono_sm'][0], 10))
    style.configure('FfpkgEdit.Treeview.Heading',
        background=COLORS['bg_3'], foreground=COLORS['fg_5'],
        relief='flat', font=('Segoe UI', 9, 'bold'))
    style.map('FfpkgEdit.Treeview',
        background=[('selected', COLORS['accent_15'])],
        foreground=[('selected', COLORS['accent_hi'])])

    cols = ('name', 'type', 'size')
    tree = ttk.Treeview(outer, columns=cols, show='headings',
                        style='FfpkgEdit.Treeview', selectmode='extended')
    tree.heading('name', text='NAME')
    tree.heading('type', text='TYPE')
    tree.heading('size', text='SIZE')
    tree.column('name', width=520, stretch=True,  anchor='w')
    tree.column('type', width=110, stretch=False, anchor='w')
    tree.column('size', width=110, stretch=False, anchor='e')

    sb = ttk.Scrollbar(outer, orient='vertical', command=tree.yview)
    tree.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    tree.pack(side='left', fill='both', expand=True)

    app._fe_listbox = _TreeListboxProxy(tree)
    app._fe_tree = tree

    # Empty-state overlay (shared component) — sits over the table and
    # is hidden the moment an image is loaded. Presentation only.
    app._fe_empty = EmptyState(outer, icon='\U0001f4e6',
        title=_('No image loaded'),
        description=_('Browse to open a .ffpkg image'), bg='bg_2')
    app._fe_empty.place(relx=0, rely=0, relwidth=1, relheight=1)

    # Footer
    foot = tk.Frame(parent, bg=COLORS['bg_3'])
    foot.pack(fill='x', pady=(1, 0))
    app._fe_sel_pill = tk.Label(foot, text='0 selected',
        font=(FONTS['mono_sm'][0], 10, 'bold'),
        bg=COLORS['bg_3'], fg=COLORS['fg_4'],
        padx=8, pady=2)
    app._fe_sel_pill.pack(side='left', padx=(10, 6), pady=4)
    app._fe_status_lbl = tk.Label(foot, textvariable=app._fe_status_var,
        font=FONTS['mono_sm'], bg=COLORS['bg_3'], fg=COLORS['fg_5'])
    app._fe_status_lbl.pack(side='right', padx=(0, 10))

    # Bindings
    tree.bind('<Double-Button-1>', lambda e: _on_enter(app))
    tree.bind('<<TreeviewSelect>>', lambda e: _on_select(app))
    tree.bind('<Return>',    lambda e: (_on_enter(app), 'break'))
    tree.bind('<Delete>',    lambda e: (_delete(app), 'break'))
    tree.bind('<BackSpace>', lambda e: (_go_up(app), 'break'))
    tree.bind('<F5>',        lambda e: (_refresh(app), 'break'))
    tree.bind('<F2>',        lambda e: (_rename(app), 'break'))
    tree.bind('<Control-a>', lambda e: _select_all(app))
    tree.bind('<Control-A>', lambda e: _select_all(app))


def _select_all(app):
    for iid in app._fe_tree.get_children(''):
        app._fe_tree.selection_add(iid)
    return 'break'


# Simple proxy mirroring tab_files.ListboxProxy but without size-string
# parsing (we always feed it (name, is_dir, size) tuples directly).
class _TreeListboxProxy:
    def __init__(self, tree):
        self._tree = tree
        self._count = 0

    def populate(self, entries):
        for iid in self._tree.get_children(''):
            self._tree.delete(iid)
        self._count = 0
        import re as _re
        # Defensive cleanup pattern: strip a leading 'inode= <NUM>' if
        # any name still has it (parser fallback safety net).
        _inode_prefix = _re.compile(r'^inode=\s*\d+\s+')
        for name, is_dir, size in entries:
            if isinstance(name, str):
                name = _inode_prefix.sub('', name)
            iid = '%05d' % self._count
            self._count += 1
            kind, type_label = _classify(name, is_dir)
            icon = _TYPE_ICONS.get(kind, '\U0001f4be')
            sz = '' if (is_dir is True or
                        (is_dir is None and '.' not in name.lower())
                       ) else _fmt_size(size)
            self._tree.insert('', 'end', iid=iid,
                values=(icon + '  ' + name, type_label, sz),
                tags=(kind,))

    def delete(self, first, last=None):
        if first == 0 and (last == 'end' or last is None):
            for iid in self._tree.get_children(''):
                self._tree.delete(iid)
            self._count = 0

    def curselection(self):
        return tuple(int(iid) for iid in self._tree.selection())


# ─────────────────────────────────────────────────────────────────────
# Inspector
# ─────────────────────────────────────────────────────────────────────
def _build_inspector(parent, app):
    head = tk.Frame(parent, bg=COLORS['bg_2'])
    head.pack(fill='x', padx=12, pady=(10, 6))
    tk.Label(head, text='INSPECTOR',
             font=(FONTS['eyebrow'][0], 9, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='left')

    hero = tk.Frame(parent, bg=COLORS['bg_2'])
    hero.pack(fill='x', padx=12, pady=(0, 8))
    app._fe_insp_icon = tk.Label(hero, text='\U0001f4be',
        font=('Segoe UI', 40),
        bg=COLORS['bg_3'], fg=COLORS['fg_3'],
        width=2, height=2,
        highlightbackground=COLORS['border_3'], highlightthickness=1)
    app._fe_insp_icon.pack(anchor='center', pady=(8, 6))
    app._fe_insp_name = tk.Label(hero, text=_('No selection'),
        font=(FONTS['mono_sm'][0], 11, 'bold'),
        bg=COLORS['bg_2'], fg=COLORS['fg_0'],
        wraplength=_INSPECTOR_WIDTH - 24, justify='center')
    app._fe_insp_name.pack(anchor='center')
    app._fe_insp_size = tk.Label(hero, text='',
        font=FONTS['mono_sm'],
        bg=COLORS['bg_2'], fg=COLORS['fg_4'])
    app._fe_insp_size.pack(anchor='center', pady=(0, 6))

    details = tk.Frame(parent, bg=COLORS['bg_2'])
    details.pack(fill='x', padx=12, pady=(0, 8))
    _detail_row(details, app, 'PATH',  mono=True, accent=True, attr='_fe_insp_path_lbl')
    _detail_row(details, app, 'TYPE',  attr='_fe_insp_type_lbl')
    _detail_row(details, app, 'SIZE',  mono=True, attr='_fe_insp_bytes_lbl')
    _detail_row(details, app, 'PERMS', mono=True, attr='_fe_insp_perms_lbl')

    # Hex preview area — populated only on Show hex click
    hex_outer = tk.Frame(parent, bg=COLORS['bg_2'])
    hex_outer.pack(fill='x', padx=12, pady=(0, 8))
    app._fe_insp_hex_btn = _ghost_btn(hex_outer,
        '\U0001f50d  ' + _('Show first 64 bytes'),
        command=lambda: _fetch_hex(app))
    app._fe_insp_hex_btn.pack(fill='x')
    app._fe_insp_hex = tk.Text(hex_outer,
        height=4, width=34, font=('Consolas', 9),
        bg=COLORS['bg_0'], fg=COLORS['fg_3'],
        relief='flat', bd=6, wrap='none', state='disabled',
        highlightbackground=COLORS['border_2'], highlightthickness=1)
    # NOT packed by default — only after Show hex is clicked

    # ── Inspector action buttons ──
    # v3.0.0: Replace and Delete were removed from the Inspector; they
    # duplicated the toolbar buttons of the same names and made the
    # right pane visually cluttered. Rename stays as the only
    # selection-specific action that benefits from being near the
    # selection details (it operates on exactly one item).
    spacer = tk.Frame(parent, bg=COLORS['bg_2'])
    spacer.pack(fill='both', expand=True)
    actions = tk.Frame(parent, bg=COLORS['bg_2'])
    actions.pack(fill='x', padx=12, pady=(8, 12), side='bottom')
    app._fe_insp_rename = _ghost_btn(actions,
        '\u270f  ' + _('Rename'),
        command=lambda: _rename(app))
    app._fe_insp_rename.pack(side='left', fill='x', expand=True)

    _clear_inspector(app)


def _detail_row(parent, app, eyebrow, mono=False, accent=False, attr=None):
    row = tk.Frame(parent, bg=COLORS['bg_2'])
    row.pack(fill='x', pady=(0, 4))
    tk.Label(row, text=eyebrow,
             font=(FONTS['eyebrow'][0], 8, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_5'],
             width=10, anchor='w').pack(side='left')
    fg = COLORS['accent_hi'] if accent else COLORS['fg_2']
    fnt = (FONTS['mono_sm'][0], 10) if mono else ('Segoe UI', 10)
    val = tk.Label(row, text='—',
        font=fnt, bg=COLORS['bg_2'], fg=fg,
        anchor='w', justify='left',
        wraplength=_INSPECTOR_WIDTH - 110)
    val.pack(side='left', fill='x', expand=True)
    if attr:
        setattr(app, attr, val)


def _clear_inspector(app):
    if hasattr(app, '_fe_insp_icon'):
        app._fe_insp_icon.config(text='\U0001f4be', fg=COLORS['fg_6'])
        app._fe_insp_name.config(text=_('No selection'), fg=COLORS['fg_4'])
        app._fe_insp_size.config(text='')
        app._fe_insp_path_lbl.config(text='—')
        app._fe_insp_type_lbl.config(text='—')
        app._fe_insp_bytes_lbl.config(text='—')
        app._fe_insp_perms_lbl.config(text='—')
        try: app._fe_insp_hex.pack_forget()
        except Exception: pass


# ─────────────────────────────────────────────────────────────────────
# Navigation
# ─────────────────────────────────────────────────────────────────────
def _refresh(app):
    img = app._fe_image_var.get()
    if not img or not os.path.isfile(img):
        return
    path = app._fe_current
    # Use cache if available
    entries = app._fe_ls_cache.get(path)
    if entries is None:
        app._fe_status_var.set(_('Listing %s...') % path)
        app.update_idletasks()
        rc, out = _run_ufs2(app, ['ls', img, path])
        if rc != 0:
            messagebox.showerror(_('ls failed'),
                _('UFS2Tool ls failed:\n\n') + out)
            return
        # Diagnostic dump — write raw ls output to %TEMP% so we can
        # debug parser issues if the format isn't what we expect.
        try:
            diag = os.path.join(tempfile.gettempdir(),
                                'ffpkg_ls_diagnostic.log')
            with open(diag, 'a', encoding='utf-8', errors='replace') as f:
                import datetime
                f.write('\n=== %s | ls %s ===\n' % (
                    datetime.datetime.now().isoformat(), path))
                f.write(out)
                f.write('\n')
        except Exception:
            pass
        entries = _parse_ls(out)
        app._fe_ls_cache[path] = entries
    # Insert ".." for non-root
    if path != '/':
        entries = [('..', True, 0)] + list(entries)
    app._fe_entries = entries
    app._fe_listbox.populate(entries)
    app._fe_path_var.set(path)
    _count = len(entries) - (1 if path != '/' else 0)
    app._fe_status_var.set(_('%d items') % _count)
    try:
        app._fe_hero.set_stat('files', '%d in folder' % _count)
    except Exception:
        pass
    _clear_inspector(app)


def _on_enter(app):
    sel = app._fe_listbox.curselection()
    if not sel:
        return
    idx = sel[0]
    if not (0 <= idx < len(app._fe_entries)):
        return
    name, is_dir, _sz = app._fe_entries[idx]
    if name == '..':
        _go_up(app)
        return
    # Probe by trying to ls the path — robust to _parse_ls
    # misclassification. If ls succeeds, it's a folder we can navigate
    # into. If ls fails, it's a file (no-op for now).
    candidate = _join_fs_path(app._fe_current, name)
    img = app._fe_image_var.get()
    if not img:
        return
    rc, _out = _run_ufs2(app, ['ls', img, candidate])
    if rc == 0:
        # ls worked — it's a folder
        app._fe_current = candidate
        _refresh(app)
    # else: silently no-op (it's a file). A future version could open
    # the file for inline editing or hex preview here.


def _go_up(app):
    if app._fe_current == '/':
        return
    app._fe_current = _dirname_fs(app._fe_current)
    _refresh(app)


def _on_select(app):
    sel = app._fe_listbox.curselection()
    _style_sel_pill(app._fe_sel_pill, len(sel))
    if not sel:
        _clear_inspector(app)
        return
    idx = sel[0]
    if not (0 <= idx < len(app._fe_entries)):
        return
    name, is_dir, size = app._fe_entries[idx]
    if name == '..':
        _clear_inspector(app)
        return
    full = _join_fs_path(app._fe_current, name)
    kind, type_label = _classify(name, is_dir)
    app._fe_insp_icon.config(text=_TYPE_ICONS.get(kind, '\U0001f4be'),
        fg=_TYPE_COLORS.get(kind, COLORS['fg_3']))
    app._fe_insp_name.config(text=name, fg=COLORS['fg_0'])
    app._fe_insp_size.config(text=_fmt_size(size) if size else '')
    app._fe_insp_path_lbl.config(text=full)
    app._fe_insp_type_lbl.config(text=type_label)
    app._fe_insp_bytes_lbl.config(
        text='%s (%d bytes)' % (_fmt_size(size), size) if size else '—')
    app._fe_insp_perms_lbl.config(text=_('(click stat to fetch)'))

    # Auto-fetch stat lazily in a thread so the UI stays responsive
    def _bg():
        img = app._fe_image_var.get()
        rc, out = _run_ufs2(app, ['stat', img, full], timeout=15)
        if rc == 0:
            # Parse Permissions line if present, fall back to first line
            perms = '—'
            for line in out.splitlines():
                ll = line.strip().lower()
                if ll.startswith('permissions') or ll.startswith('mode'):
                    perms = line.split(':', 1)[-1].strip()
                    break
            app.after(0, lambda: app._fe_insp_perms_lbl.config(text=perms))
    threading.Thread(target=_bg, daemon=True).start()

    # Reset hex pane
    try:
        app._fe_insp_hex.pack_forget()
        app._fe_insp_hex_btn.config(state='normal' if not is_dir else 'disabled')
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────
# Operations
# ─────────────────────────────────────────────────────────────────────
def _add_files(app):
    if not _has_image(app):
        return
    paths = filedialog.askopenfilenames(
        title=_('Select files to add'))
    if not paths:
        return
    img = app._fe_image_var.get()
    errors = []
    added = 0
    replaced = 0

    # Always additive: if a same-named file exists, replace it; else add.
    for src in paths:
        basename = os.path.basename(src)
        dst = _join_fs_path(app._fe_current, basename)
        exists, _is_dir = _name_exists(app, basename)
        if exists:
            rc, out = _run_ufs2(app, ['replace', img, dst, src])
            if rc != 0:
                # Replace failed (maybe existing was a folder?) — record
                errors.append(basename + ' (replace): ' + out.strip()[:200])
            else:
                replaced += 1
        else:
            rc, out = _run_ufs2(app, ['add', img, dst, src])
            if rc != 0:
                errors.append(basename + ': ' + out.strip()[:200])
            else:
                added += 1

    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if errors:
        messagebox.showerror(_('Add failed'),
            '\n'.join(errors[:5]) + ('\n...' if len(errors) > 5 else ''))
    else:
        parts = []
        if added:    parts.append(_('Added %d') % added)
        if replaced: parts.append(_('replaced %d') % replaced)
        app._fe_status_var.set(', '.join(parts) if parts else _('Done'))


def _add_folder(app):
    if not _has_image(app):
        return
    src = filedialog.askdirectory(title=_('Select folder to add'))
    if not src:
        return
    img = app._fe_image_var.get()
    folder_name = os.path.basename(os.path.normpath(src))
    dst = _join_fs_path(app._fe_current, folder_name)

    # Always additive: if a folder with this name already exists, merge
    # the new files into it. If it doesn't exist, create it.
    exists, _unused = _name_exists(app, folder_name)
    if exists:
        # Additive merge — no confirmation needed, this is non-destructive
        app._fe_status_var.set(_('Merging folder...'))
        app.update_idletasks()
        stats = {'added': 0, 'replaced': 0, 'errors': []}
        _merge_into(app, img, src, dst, stats)
        _invalidate_cache(app, app._fe_current)
        _refresh(app)
        if stats['errors']:
            err_msg = '\n'.join(stats['errors'][:5])
            if len(stats['errors']) > 5:
                err_msg += '\n...'
            diag_path = getattr(app, '_fe_diag_path', '')
            if diag_path:
                err_msg += '\n\n' + _('Diagnostic log:\n') + diag_path
            messagebox.showerror(_('Merge had errors'), err_msg)
        else:
            parts = []
            if stats['added']:    parts.append(_('added %d') % stats['added'])
            if stats['replaced']: parts.append(_('replaced %d') % stats['replaced'])
            app._fe_status_var.set(_('Merged: ') +
                (', '.join(parts) if parts else _('no changes')))
        return

    # New folder — just add the whole tree
    rc, out = _run_ufs2(app, ['add', img, dst, src], timeout=600)
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if rc != 0:
        messagebox.showerror(_('Add folder failed'), out)
    else:
        app._fe_status_var.set(_('Added folder: ') + folder_name)


def _merge_into(app, img, local_dir, fs_dir, stats):
    """Recursively merge `local_dir` (on disk) into `fs_dir` (inside img).
    For each item in local_dir:
      - file + same-named file exists in fs_dir   -> replace
      - file + nothing in fs_dir at that name     -> add
      - folder + same-named folder exists         -> recurse
      - folder + nothing exists at that name      -> add (whole tree)
      - mismatch                                  -> skip + record

    NON-DESTRUCTIVE GUARANTEE: this function never calls `delete`. The
    only operations it issues are `ls` (read-only), `replace` (one-file
    swap), and `add` (one-file/one-folder insert). Items that exist in
    fs_dir but NOT in local_dir are physically untouched.
    """
    # Snapshot what's in the image at fs_dir
    rc, out = _run_ufs2(app, ['ls', img, fs_dir])
    if rc != 0:
        stats['errors'].append('ls %s: %s' % (fs_dir, out.strip()[:200]))
        _diag(app, 'ls FAILED %s rc=%d\n%s\n' % (fs_dir, rc, out))
        return
    parsed = _parse_ls(out)
    existing = {}
    for name, is_dir, _sz in parsed:
        existing[name] = is_dir
    # Diagnostic dump — raw output + parsed result, so we can debug
    # parsing problems against the real UFS2Tool behavior.
    _diag(app, '─── ls %s ───\nRAW OUTPUT (%d bytes):\n%s\nPARSED:\n%s\n'
          % (fs_dir, len(out), out,
             '\n'.join('  %s %s %d' % ('DIR' if d else 'FILE', n, s)
                       for (n, d, s) in parsed)))

    try:
        local_entries = os.listdir(local_dir)
    except Exception as e:
        stats['errors'].append('readdir %s: %s' % (local_dir, e))
        return

    for entry in local_entries:
        src_path = os.path.join(local_dir, entry)
        dst_path = _join_fs_path(fs_dir, entry)
        try:
            is_dir_src = os.path.isdir(src_path)
        except Exception:
            continue

        existing_is_dir = existing.get(entry)
        if entry not in existing:
            # No collision: add the whole thing (file or recursive folder)
            _diag(app, 'ADD %s (not in image)\n' % dst_path)
            rc, out = _run_ufs2(app, ['add', img, dst_path, src_path],
                                timeout=600)
            if rc != 0:
                stats['errors'].append('add %s: %s'
                                        % (dst_path, out.strip()[:200]))
            else:
                stats['added'] += 1
        elif is_dir_src and existing_is_dir:
            # Folder onto folder -> recurse
            _diag(app, 'RECURSE %s (folder onto folder)\n' % dst_path)
            _merge_into(app, img, src_path, dst_path, stats)
        elif (not is_dir_src) and (existing_is_dir is False):
            # File onto file -> replace in place
            _diag(app, 'REPLACE %s (file onto file)\n' % dst_path)
            rc, out = _run_ufs2(app,
                ['replace', img, dst_path, src_path])
            if rc != 0:
                stats['errors'].append('replace %s: %s'
                                        % (dst_path, out.strip()[:200]))
            else:
                stats['replaced'] += 1
        elif is_dir_src and existing_is_dir is False:
            # Source is folder, existing matched as file — likely a
            # parser misclassification. Try treating as folder first:
            # recurse. If the recursive ls fails, it'll record an error
            # but not destroy anything.
            _diag(app,
                'AMBIGUOUS %s (src=folder, existing parsed as file - recursing anyway)\n'
                % dst_path)
            _merge_into(app, img, src_path, dst_path, stats)
        elif (not is_dir_src) and existing_is_dir:
            # File onto folder — genuine type mismatch. Skip safely.
            _diag(app, 'SKIP %s (file onto folder)\n' % dst_path)
            stats['errors'].append(
                _('Skipped %s: source is a file but image has a folder')
                % dst_path)
        else:
            # Existing not in listing (parser issue) — try replace as a
            # best-effort.
            _diag(app, 'AMBIGUOUS %s (existing_is_dir=%r) - trying replace\n'
                  % (dst_path, existing_is_dir))
            rc, out = _run_ufs2(app,
                ['replace', img, dst_path, src_path])
            if rc != 0:
                # Replace failed — fall back to add
                rc2, out2 = _run_ufs2(app,
                    ['add', img, dst_path, src_path])
                if rc2 != 0:
                    stats['errors'].append('replace+add %s: %s'
                                            % (dst_path, out2.strip()[:200]))
                else:
                    stats['added'] += 1
            else:
                stats['replaced'] += 1


def _diag(app, line):
    """Append a line to the diagnostic log for the current merge session.
    File: <temp>/ffpkg_merge_diagnostic.log
    """
    try:
        path = getattr(app, '_fe_diag_path', None)
        if path is None:
            path = os.path.join(tempfile.gettempdir(),
                                 'ffpkg_merge_diagnostic.log')
            app._fe_diag_path = path
        with open(path, 'a', encoding='utf-8', errors='replace') as f:
            f.write(line)
    except Exception:
        pass


def _diag_start(app):
    """Reset the diagnostic log at the start of a merge."""
    try:
        path = os.path.join(tempfile.gettempdir(),
                             'ffpkg_merge_diagnostic.log')
        with open(path, 'w', encoding='utf-8') as f:
            import datetime
            f.write('exFAT Image Builder \u2014 ffpkg merge diagnostic\n')
            f.write('Time: %s\n' % datetime.datetime.now().isoformat())
            f.write('Image: %s\n' % app._fe_image_var.get())
            f.write('\n')
        app._fe_diag_path = path
    except Exception:
        pass


def _new_folder(app):
    if not _has_image(app):
        return
    from tkinter import simpledialog
    name = simpledialog.askstring(_('New folder'),
        _('Folder name:\n\n(Note: a .keep placeholder file will be\n'
          'created inside the new folder, since UFS2Tool has no\n'
          'native mkdir command.)'))
    if not name:
        return
    name = name.strip().strip('/')
    if not name:
        return
    img = app._fe_image_var.get()
    # Collision check
    exists, is_dir = _name_exists(app, name)
    if exists:
        choice = _ask_overwrite(app, name, is_dir, allow_apply_to_all=False)
        if choice in ('cancel', 'skip'):
            return
        rc, out = _run_ufs2(app, ['delete', img,
                                  _join_fs_path(app._fe_current, name)],
                            timeout=300)
        if rc != 0:
            messagebox.showerror(_('Overwrite failed'),
                _('Could not delete existing item:\n\n') + out)
            return
    dst = _join_fs_path(app._fe_current, name, '.keep')
    # Create a tiny temp placeholder file
    fd, tmppath = tempfile.mkstemp(prefix='ffpkg_keep_', suffix='.txt')
    try:
        os.write(fd, b'# UFS2Tool placeholder - keep this file\n')
        os.close(fd)
        rc, out = _run_ufs2(app, ['add', img, dst, tmppath])
    finally:
        try: os.unlink(tmppath)
        except Exception: pass
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if rc != 0:
        messagebox.showerror(_('New folder failed'), out)
    else:
        app._fe_status_var.set(_('Created folder: ') + name)


def _replace_file(app):
    if not _has_image(app):
        return
    sel = app._fe_listbox.curselection()
    if not sel:
        messagebox.showwarning(_('No selection'),
            _('Select a file to replace.'))
        return
    idx = sel[0]
    name, is_dir, _sz = app._fe_entries[idx]
    if is_dir or name == '..':
        messagebox.showwarning(_('Not a file'),
            _('Select a file, not a folder.'))
        return
    src = filedialog.askopenfilename(
        title=_('Select replacement file'),
        initialfile=name)
    if not src:
        return
    img = app._fe_image_var.get()
    dst = _join_fs_path(app._fe_current, name)
    rc, out = _run_ufs2(app, ['replace', img, dst, src])
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if rc != 0:
        messagebox.showerror(_('Replace failed'), out)
    else:
        app._fe_status_var.set(_('Replaced: ') + name)


def _delete(app):
    if not _has_image(app):
        return
    sel = app._fe_listbox.curselection()
    if not sel:
        return
    # Resolve names, skip ..
    targets = []
    for idx in sel:
        if 0 <= idx < len(app._fe_entries):
            name, is_dir, _sz = app._fe_entries[idx]
            if name == '..':
                continue
            targets.append((name, is_dir))
    if not targets:
        return
    if not messagebox.askyesno(_('Delete'),
            _('Delete %d item(s) from the image?\n\n') % len(targets) +
            '\n'.join(t[0] for t in targets[:10]) +
            ('\n...' if len(targets) > 10 else '')):
        return
    img = app._fe_image_var.get()
    errors = []
    for name, _is_dir in targets:
        dst = _join_fs_path(app._fe_current, name)
        rc, out = _run_ufs2(app, ['delete', img, dst])
        if rc != 0:
            errors.append(name + ': ' + out.strip()[:200])
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if errors:
        messagebox.showerror(_('Delete failed'),
            '\n'.join(errors[:5]) + ('\n...' if len(errors) > 5 else ''))
    else:
        app._fe_status_var.set(_('Deleted %d item(s)') % len(targets))


def _rename(app):
    if not _has_image(app):
        return
    sel = app._fe_listbox.curselection()
    if not sel:
        messagebox.showwarning(_('No selection'),
            _('Select an item to rename.'))
        return
    idx = sel[0]
    name, _is_dir, _sz = app._fe_entries[idx]
    if name == '..':
        return
    from tkinter import simpledialog
    new_name = simpledialog.askstring(_('Rename'),
        _('New name for "%s":') % name, initialvalue=name)
    if not new_name or new_name == name:
        return
    new_name = new_name.strip().strip('/')
    if not new_name:
        return
    # Collision check (skip if renaming to the same name)
    exists, existing_is_dir = _name_exists(app, new_name)
    if exists and new_name != name:
        choice = _ask_overwrite(app, new_name, existing_is_dir,
                                 allow_apply_to_all=False)
        if choice in ('cancel', 'skip'):
            return
        rc, out = _run_ufs2(app, ['delete', app._fe_image_var.get(),
                                  _join_fs_path(app._fe_current, new_name)],
                            timeout=300)
        if rc != 0:
            messagebox.showerror(_('Overwrite failed'),
                _('Could not delete existing item:\n\n') + out)
            return
    img = app._fe_image_var.get()
    src = _join_fs_path(app._fe_current, name)
    rc, out = _run_ufs2(app, ['rename', img, src, new_name])
    _invalidate_cache(app, app._fe_current)
    _refresh(app)
    if rc != 0:
        messagebox.showerror(_('Rename failed'), out)
    else:
        app._fe_status_var.set(_('Renamed to: ') + new_name)


def _fetch_hex(app):
    """Extract the selected file to a temp dir and read first 64 bytes."""
    sel = app._fe_listbox.curselection()
    if not sel:
        return
    idx = sel[0]
    name, is_dir, _sz = app._fe_entries[idx]
    if is_dir or name == '..':
        return
    img = app._fe_image_var.get()
    fs_path = _join_fs_path(app._fe_current, name)
    app._fe_status_var.set(_('Fetching hex preview...'))
    app.update_idletasks()

    def _bg():
        tmpdir = tempfile.mkdtemp(prefix='ffpkg_hex_')
        try:
            rc, out = _run_ufs2(app, ['extract', img, tmpdir, fs_path], timeout=60)
            if rc != 0:
                app.after(0, app._fe_status_var.set,
                    _('Hex fetch failed: ') + out.strip()[:120])
                return
            # Find the extracted file (UFS2Tool preserves the fs path)
            found = None
            for root, _dirs, files in os.walk(tmpdir):
                for f in files:
                    if f == name:
                        found = os.path.join(root, f)
                        break
                if found:
                    break
            if not found:
                app.after(0, app._fe_status_var.set,
                    _('Hex fetch: file not found after extract'))
                return
            with open(found, 'rb') as f:
                data = f.read(64)
            lines = []
            for off in range(0, len(data), 16):
                chunk = data[off:off + 16]
                left  = ' '.join('%02x' % b for b in chunk[:8])
                right = ' '.join('%02x' % b for b in chunk[8:])
                lines.append('%08x  %-23s  %s' % (off, left, right))
            hex_text = '\n'.join(lines)
            def _show():
                app._fe_insp_hex.config(state='normal')
                app._fe_insp_hex.delete('1.0', 'end')
                app._fe_insp_hex.insert('end', hex_text)
                app._fe_insp_hex.config(state='disabled')
                app._fe_insp_hex.pack(fill='x', pady=(4, 0))
                app._fe_status_var.set(_('Hex preview loaded'))
            app.after(0, _show)
        finally:
            try:
                import shutil
                shutil.rmtree(tmpdir, ignore_errors=True)
            except Exception:
                pass
    threading.Thread(target=_bg, daemon=True).start()


# ─────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────
def _has_image(app):
    img = app._fe_image_var.get()
    if not img or not os.path.isfile(img):
        messagebox.showwarning(_('No image'),
            _('Browse for a .ffpkg image first.'))
        return False
    return True


def _name_exists(app, name):
    """Check whether `name` is already present in the current directory.
    Returns (exists, is_dir). False/False if not found."""
    for entry_name, is_dir, _sz in app._fe_entries:
        if entry_name == name:
            return True, is_dir
    return False, False


def _ask_overwrite(app, name, is_dir, allow_apply_to_all=False,
                   allow_merge=False):
    """Ask the user what to do about a name collision.
    Returns one of: 'overwrite', 'skip', 'cancel', 'overwrite_all',
    'skip_all', 'merge', 'merge_all'. 'merge' is only returned when
    allow_merge=True.
    """
    from tkinter import Toplevel
    kind = _('folder') if is_dir else _('file')
    result = {'choice': 'cancel'}

    dlg = Toplevel(app)
    dlg.title(_('Item already exists'))
    dlg.configure(bg=COLORS['bg_1'])
    dlg.transient(app)
    dlg.grab_set()
    dlg.resizable(False, False)

    dlg_w = 560 if allow_merge else 420
    dlg_h = 260 if allow_merge else 200
    dlg.update_idletasks()
    try:
        x = app.winfo_x() + (app.winfo_width() - dlg_w) // 2
        y = app.winfo_y() + (app.winfo_height() - dlg_h) // 2
        dlg.geometry('%dx%d+%d+%d' % (dlg_w, dlg_h, max(0, x), max(0, y)))
    except Exception:
        dlg.geometry('%dx%d' % (dlg_w, dlg_h))

    head = tk.Frame(dlg, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=16, pady=(14, 4))
    tk.Label(head, text='\u26a0',
             font=('Segoe UI', 16),
             bg=COLORS['bg_1'], fg=COLORS['warn']
             ).pack(side='left', padx=(0, 8))
    tk.Label(head,
             text=_('A %s named "%s" already exists in this folder.')
                  % (kind, name),
             font=('Segoe UI', 10, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0'],
             wraplength=dlg_w - 60, justify='left'
             ).pack(side='left', fill='x', expand=True)

    if allow_merge:
        # Two-line explanation block, each with a clear callout
        block = tk.Frame(dlg, bg=COLORS['bg_1'])
        block.pack(fill='x', padx=18, pady=(4, 8))
        _opt_row(block, '\u2713', COLORS['success_hi'],
            _('Merge'),
            _('Replace files that exist in both. Keep everything '
              'else in the existing folder.'))
        _opt_row(block, '\u26a0', COLORS['danger_hi'],
            _('Replace entirely'),
            _('Delete the existing folder first. Everything inside '
              'is lost \u2014 including files NOT in your new folder.'))
    else:
        tk.Label(dlg,
            text=_('Overwriting will delete the existing %s first.') % kind,
            font=FONTS['mono_sm'],
            bg=COLORS['bg_1'], fg=COLORS['fg_5'],
            wraplength=dlg_w - 40, justify='left'
            ).pack(fill='x', padx=18, pady=(0, 8))

    apply_var = tk.BooleanVar(value=False)
    if allow_apply_to_all:
        tk.Checkbutton(dlg,
            text=_('Apply this choice to remaining conflicts'),
            variable=apply_var,
            font=FONTS['mono_sm'],
            bg=COLORS['bg_1'], fg=COLORS['fg_3'],
            activebackground=COLORS['bg_1'],
            selectcolor=COLORS['bg_2'],
            cursor='hand2'
            ).pack(anchor='w', padx=18, pady=(0, 6))

    # Buttons — Merge primary (accent), Replace entirely demoted to ghost
    # with extra confirmation step to prevent accidental data loss
    btns = tk.Frame(dlg, bg=COLORS['bg_1'])
    btns.pack(fill='x', padx=16, pady=(6, 14), side='bottom')

    def _pick(c):
        if apply_var.get() and c in ('overwrite', 'skip', 'merge'):
            result['choice'] = c + '_all'
        else:
            result['choice'] = c
        dlg.destroy()

    def _confirm_replace():
        """Replace entirely needs a separate confirmation step — it's
        the destructive choice and shouldn't be one click away."""
        from tkinter import messagebox
        if messagebox.askyesno(
                _('Confirm Replace entirely'),
                _('Are you sure?\n\n'
                  'The existing folder "%s" will be deleted, including '
                  'any files NOT in your new folder.\n\n'
                  'This cannot be undone.') % name,
                icon='warning', default='no', parent=dlg):
            _pick('overwrite')

    # Right side: Cancel + Skip (low-risk)
    tk.Button(btns, text=_('Cancel'),
        font=(FONTS['button'][0], 9),
        bg=COLORS['bg_3'], fg=COLORS['fg_2'],
        activebackground=COLORS['bg_5'],
        relief='flat', bd=0, padx=12, pady=6, cursor='hand2',
        highlightbackground=COLORS['border_3'], highlightthickness=1,
        command=lambda: _pick('cancel')).pack(side='right')
    tk.Button(btns, text=_('Skip'),
        font=(FONTS['button'][0], 9),
        bg=COLORS['bg_3'], fg=COLORS['fg_2'],
        activebackground=COLORS['bg_5'],
        relief='flat', bd=0, padx=12, pady=6, cursor='hand2',
        highlightbackground=COLORS['border_3'], highlightthickness=1,
        command=lambda: _pick('skip')).pack(side='right', padx=(0, 6))

    if allow_merge:
        # Left side: Replace entirely (subtle ghost, with extra confirm)
        tk.Button(btns,
            text='\U0001f5d1  ' + _('Replace entirely'),
            font=(FONTS['button'][0], 9),
            bg=COLORS['bg_3'], fg=COLORS['danger_hi'],
            activebackground=COLORS['bg_5'],
            relief='flat', bd=0, padx=12, pady=6, cursor='hand2',
            highlightbackground=COLORS['border_3'], highlightthickness=1,
            command=_confirm_replace).pack(side='left')
        # Merge primary, right of Replace (so it sits next to Skip/Cancel)
        merge_btn = tk.Button(btns,
            text='\u2713  ' + _('Merge'),
            font=(FONTS['button'][0], 9, 'bold'),
            bg=COLORS['accent'], fg=COLORS['fg_0'],
            activebackground=COLORS['accent_hi'],
            relief='flat', bd=0, padx=14, pady=6, cursor='hand2',
            command=lambda: _pick('merge'))
        merge_btn.pack(side='right', padx=(0, 6))
        # Enter activates Merge
        dlg.bind('<Return>', lambda e: _pick('merge'))
        merge_btn.focus_set()
    else:
        # Files (non-folder): plain Overwrite stays as the primary action
        ow_btn = tk.Button(btns,
            text=_('Overwrite'),
            font=(FONTS['button'][0], 9, 'bold'),
            bg=COLORS['warn'], fg='#000000',
            activebackground=COLORS['warn_hi'],
            relief='flat', bd=0, padx=12, pady=6, cursor='hand2',
            command=lambda: _pick('overwrite'))
        ow_btn.pack(side='right', padx=(0, 6))
        dlg.bind('<Return>', lambda e: _pick('overwrite'))
        ow_btn.focus_set()

    dlg.bind('<Escape>', lambda e: _pick('cancel'))
    dlg.wait_window()
    return result['choice']


def _opt_row(parent, icon, icon_fg, title, body):
    """One option-explanation row inside the overwrite dialog."""
    row = tk.Frame(parent, bg=COLORS['bg_1'])
    row.pack(fill='x', pady=(2, 4))
    tk.Label(row, text=icon,
        font=('Segoe UI', 11, 'bold'),
        bg=COLORS['bg_1'], fg=icon_fg,
        width=2
        ).pack(side='left', anchor='n')
    col = tk.Frame(row, bg=COLORS['bg_1'])
    col.pack(side='left', fill='x', expand=True)
    tk.Label(col, text=title,
        font=('Segoe UI', 10, 'bold'),
        bg=COLORS['bg_1'], fg=COLORS['fg_1'],
        anchor='w').pack(anchor='w')
    tk.Label(col, text=body,
        font=FONTS['mono_sm'],
        bg=COLORS['bg_1'], fg=COLORS['fg_4'],
        anchor='w', justify='left', wraplength=480
        ).pack(anchor='w')


def _invalidate_cache(app, path):
    """Drop the cached listing for the given path (and any descendants)."""
    keys_to_drop = [k for k in app._fe_ls_cache
                    if k == path or k.startswith(path.rstrip('/') + '/')]
    for k in keys_to_drop:
        del app._fe_ls_cache[k]


# ─────────────────────────────────────────────────────────────────────
# Button helpers
# ─────────────────────────────────────────────────────────────────────
def _ghost_btn(parent, text, command):
    return tk.Button(parent, text=text,
        font=(FONTS['button'][0], 9),
        bg=COLORS['bg_3'], fg=COLORS['fg_2'],
        activebackground=COLORS['bg_5'], activeforeground=COLORS['fg_0'],
        relief='flat', bd=0, padx=12, pady=6, cursor='hand2',
        highlightbackground=COLORS['border_3'], highlightthickness=1,
        command=command)


def _accent_btn(parent, text, command):
    return tk.Button(parent, text=text,
        font=(FONTS['button'][0], 9, 'bold'),
        bg=COLORS['accent'], fg=COLORS['fg_0'],
        activebackground=COLORS['accent_hi'], activeforeground=COLORS['fg_0'],
        relief='flat', bd=0, padx=12, pady=6, cursor='hand2',
        command=command)


def _danger_btn(parent, text, command):
    return tk.Button(parent, text=text,
        font=(FONTS['button'][0], 9, 'bold'),
        bg=COLORS['danger'], fg=COLORS['fg_0'],
        activebackground=COLORS['danger_hi'], activeforeground=COLORS['fg_0'],
        disabledforeground=COLORS['fg_5'],
        relief='flat', bd=0, padx=12, pady=6, cursor='hand2',
        command=command)


def _icon_btn(parent, text, command, kind='ghost'):
    if kind == 'accent':
        return _accent_btn(parent, text, command)
    if kind == 'danger':
        fg = COLORS['danger_hi']
    else:
        fg = COLORS['fg_2']
    return tk.Button(parent, text=text,
        font=(FONTS['button'][0], 9),
        bg=COLORS['bg_2'], fg=fg,
        activebackground=COLORS['bg_3'], activeforeground=fg,
        relief='flat', bd=0, padx=10, pady=5, cursor='hand2',
        command=command)

# ─────────────────────────────────────────────────────────────────────
# Apply Backport (v2.0.6) — overlay a user-made backport folder onto
# the currently-open .ffpkg image, backing up every replaced original.
#
# Workflow:
#   1) Ask the user to pick the backport SOURCE folder (the folder that
#      contains the patched files they've produced — typically the
#      output of Auto Backport or a hand-edited build).
#   2) Walk the source recursively. For every file, compute its target
#      path inside the image, anchored at the current breadcrumb
#      directory (so a user can drop a backport "into" /sce_sys, /,
#      /Media, etc. without rebuilding the folder layout).
#   3) For each target that ALREADY EXISTS as a file in the image:
#         a. UFS2Tool extract  -> <backup_dir>/<rel_path>
#         b. UFS2Tool replace  -> patched file
#      For each target that DOES NOT exist:
#         - UFS2Tool add       -> patched file (no backup needed)
#      Items that exist in the image but NOT in the backport folder
#      are PHYSICALLY UNTOUCHED. No deletes are ever issued.
#   4) Zip the backup folder into <image_dir>/<image_stem>.backup-<ts>.zip
#      with a backup_manifest.txt at the root.
#   5) Show a summary dialog with the counts and the path to the zip.
# ─────────────────────────────────────────────────────────────────────
import zipfile as _backport_zipfile  # local alias to avoid shadowing

def _apply_backport(app):
    """Toolbar entry-point. Asks what kind of source the user has
    (folder / .ffpkg / .exfat), materialises it to a real folder on
    disk if needed, then runs the overlay on a background thread."""
    if not _has_image(app):
        return
    img = app._fe_image_var.get()

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
            messagebox.showerror(_('Bad folder'),
                _('Selected path is not a folder.'))
            return
    elif kind == 'ffpkg':
        src_path = filedialog.askopenfilename(
            title=_('Select source .ffpkg'),
            filetypes=[('FFPKG image', '*.ffpkg'),
                       ('All files',   '*.*')])
        if not src_path:
            return
        if os.path.abspath(src_path) == os.path.abspath(img):
            messagebox.showerror(_('Same image'),
                _('Source .ffpkg is the same file that is currently '
                  'open for editing. Pick a different image.'))
            return
    elif kind == 'exfat':
        src_path = filedialog.askopenfilename(
            title=_('Select source exFAT image'),
            filetypes=[('exFAT image', '*.exfat;*.img'),
                       ('All files',   '*.*')])
        if not src_path:
            return
    else:
        return

    # ── 3. Materialise source to a real folder on disk ──
    #    For 'folder' this is the path itself. For 'ffpkg' / 'exfat',
    #    extract the whole tree into a temp dir, which we'll clean up
    #    at the end.
    app._fe_status_var.set(_('Preparing backport source...'))
    app.update_idletasks()
    try:
        src_dir, cleanup_fn = _bp_materialize_source(app, kind, src_path)
    except Exception as e:
        messagebox.showerror(_('Source preparation failed'), str(e))
        app._fe_status_var.set(_('Backport cancelled.'))
        return
    if not src_dir:
        app._fe_status_var.set(_('Backport cancelled.'))
        return

    # ── 4. Pre-scan the materialised source ──
    try:
        items = _scan_backport_source(src_dir)
    except Exception as e:
        try:
            cleanup_fn()
        except Exception:
            pass
        messagebox.showerror(_('Scan failed'), str(e))
        return
    if not items:
        try:
            cleanup_fn()
        except Exception:
            pass
        messagebox.showwarning(_('Empty backport'),
            _('No files were found in the selected source.'))
        return

    # Confirmation — make it clear what will happen and where backups go.
    img_dir  = os.path.dirname(img)
    img_stem = os.path.splitext(os.path.basename(img))[0]
    ts       = time.strftime('%Y%m%d-%H%M%S')
    backup_zip = os.path.join(img_dir, '%s.backup-%s.zip' % (img_stem, ts))
    kind_label = {
        'folder': _('Folder'),
        'ffpkg':  _('.ffpkg image'),
        'exfat':  _('exFAT image'),
    }.get(kind, kind)

    # Pre-flight: need ~2× the image size free on the destination drive
    # for the extracted dump + the rebuilt image. Warn if we don't have
    # it (don't block — disk-free reporting is finicky on some FS types).
    try:
        img_size  = os.path.getsize(img)
        free_now  = _bp_free_space(img_dir)
        needed    = int(img_size * 2.1) + (64 * 1024 * 1024)   # +64MB slack
        space_warn = ''
        if free_now is not None and free_now < needed:
            space_warn = ('\n\n' +
                _('\u26a0  Low disk space warning: this needs about '
                  '%s free on %s but only %s seems to be available. '
                  'The rebuild may run out of space partway through.')
                % (_bp_fmt_size(needed), img_dir, _bp_fmt_size(free_now)))
    except Exception:
        space_warn = ''
        img_size   = 0

    msg = (_('Apply backport to image (rebuild)?') + '\n\n' +
           _('Source kind: ') + kind_label + '\n' +
           _('Source: ') + src_path + '\n' +
           _('Target dir inside image: ') + (app._fe_current or '/') + '\n' +
           _('Files in backport: ') + str(len(items)) + '\n\n' +
           _('How it works:') + '\n' +
           _('  1. The image is fully extracted to a temp folder.') + '\n' +
           _('  2. Your backport is copied onto the extracted dump.') + '\n' +
           _('  3. UFS2Tool newfs rebuilds the .ffpkg from scratch') + '\n' +
           _('     \u2014 same flags as the regular Build flow.') + '\n' +
           _('  4. The original .ffpkg is replaced atomically.') + '\n\n' +
           _('The complete original .ffpkg will be saved as:') + '\n' +
           backup_zip + space_warn + '\n\n' +
           _('Nothing is overwritten in the image until the rebuild '
             'succeeds. If the rebuild fails, your original .ffpkg '
             'stays as it was.'))
    if not messagebox.askyesno(_('Apply backport (rebuild)'), msg):
        try:
            cleanup_fn()
        except Exception:
            pass
        return

    app._fe_status_var.set(_('Preparing rebuild...'))
    app.update_idletasks()

    # ── Progress dialog ──
    prog = _RebuildProgress(app, _('Apply backport — rebuild'))

    def _bg():
        stats = {
            'extracted_files': 0,
            'overlay_added':   0,
            'overlay_replaced': 0,
            'errors':          [],
            'rebuilt':         False,
            'swapped':         False,
            'extract_errors':  0,
        }

        # Working dirs:
        #   work_dir/dump/    — extracted contents of the original image
        #   work_dir/new.img  — rebuilt image (renamed to .ffpkg on success)
        work_dir = tempfile.mkdtemp(prefix='ffpkg_rebuild_')
        dump_dir = os.path.join(work_dir, 'dump')
        new_img  = os.path.join(work_dir, 'new.ffpkg')

        # Shared flag for the background poll thread so we can shut it
        # down between stages without leaking a thread.
        poll_state = {'stop': False, 'dir': None, 'cb': None}

        def _poll_loop():
            """Counts files+dirs under poll_state['dir'] and pushes the
            count to whatever callback poll_state['cb'] is set to. Used
            for the extract stage (we don't get progress from UFS2Tool
            itself, so we just watch the disk fill up)."""
            import time as _t
            while not poll_state['stop']:
                d  = poll_state['dir']
                cb = poll_state['cb']
                if d and cb and os.path.isdir(d):
                    try:
                        n_files = 0
                        n_bytes = 0
                        for _r, _ds, _fs in os.walk(d):
                            n_files += len(_fs)
                            for f in _fs:
                                try:
                                    n_bytes += os.path.getsize(
                                        os.path.join(_r, f))
                                except Exception:
                                    pass
                        cb(n_files, n_bytes)
                    except Exception:
                        pass
                _t.sleep(0.7)

        poll_thread = threading.Thread(target=_poll_loop, daemon=True,
            name='ffpkg-rebuild-poll')
        poll_thread.start()

        def _cleanup_work():
            try:
                import shutil as _sh
                _sh.rmtree(work_dir, ignore_errors=True)
            except Exception:
                pass

        try:
            # ── Step 1: zip the original .ffpkg as the user's safety net ──
            app.after(0, prog.set_stage, 'backup',
                _('Backing up original .ffpkg...'))
            app.after(0, app._fe_status_var.set,
                _('Backing up original .ffpkg...'))
            try:
                img_size_for_progress = os.path.getsize(img)
            except Exception:
                img_size_for_progress = 0
            try:
                _bp_zip_single_file(img, backup_zip,
                    progress_cb=lambda done, total: app.after(0,
                        prog.set_stage_progress,
                        (done * 100.0 / total) if total else 100.0,
                        _('%s / %s') % (_bp_fmt_size(done),
                                        _bp_fmt_size(total))))
            except Exception as e:
                raise RuntimeError(
                    _('Failed to back up original image: ') + str(e))
            app.after(0, prog.set_stage_progress, 100.0)

            # ── Step 2: extract the whole image to dump_dir ──
            app.after(0, prog.set_stage, 'extract',
                _('Extracting image (this can take a while)...'))
            app.after(0, app._fe_status_var.set,
                _('Extracting image...'))
            os.makedirs(dump_dir, exist_ok=True)

            # Estimate target size for the extract progress bar from
            # the image size — the on-disk dump will end up slightly
            # smaller (no filesystem overhead) but it's a reasonable
            # ceiling for the bar.
            ex_target_bytes = max(1, img_size_for_progress)
            def _extract_progress_cb(n_files, n_bytes):
                pct = (n_bytes * 100.0 / ex_target_bytes) if ex_target_bytes \
                                                          else 0.0
                pct = max(0.0, min(99.0, pct))   # never hit 100 here
                app.after(0, prog.set_stage_progress, pct,
                    _('%(files)d files extracted  •  %(bytes)s')
                    % {'files': n_files, 'bytes': _bp_fmt_size(n_bytes)})

            poll_state['dir'] = dump_dir
            poll_state['cb']  = _extract_progress_cb

            ex_rc, ex_out = _run_ufs2(app, ['extract', img, dump_dir],
                                       timeout=3600)
            if ex_rc != 0:
                ex_rc, ex_out = _run_ufs2(app,
                    ['extract', img, dump_dir, '/'], timeout=3600)

            # Detach poll from extract dir before reading final count.
            poll_state['cb'] = None
            if ex_rc != 0:
                raise RuntimeError(
                    _('Image extraction failed: ') +
                    (ex_out or '').strip()[:500])
            # Final count for the summary dialog.
            extracted_count = 0
            try:
                for _r, _d, _fs in os.walk(dump_dir):
                    extracted_count += len(_fs)
            except Exception:
                pass
            stats['extracted_files'] = extracted_count
            stats['extract_errors']  = 0
            if extracted_count == 0:
                raise RuntimeError(
                    _('Image extraction produced no files. ') +
                    _('Cannot rebuild.'))
            app.after(0, prog.set_stage_progress, 100.0,
                _('%d files extracted') % extracted_count)

            # ── Step 3: overlay the backport onto dump_dir ──
            app.after(0, prog.set_stage, 'overlay',
                _('Applying backport to extracted dump...'))
            app.after(0, app._fe_status_var.set,
                _('Applying backport to extracted dump...'))
            base_fs_dir = app._fe_current or '/'
            anchor_rel = base_fs_dir.strip('/').replace('/', os.sep)
            anchor_abs = (os.path.join(dump_dir, anchor_rel)
                          if anchor_rel else dump_dir)
            try:
                _bp_overlay_into_dump(items, anchor_abs, stats,
                    progress_cb=lambda done, total, name: app.after(0,
                        prog.set_stage_progress,
                        (done * 100.0 / total) if total else 100.0,
                        _('%(done)d / %(total)d  •  %(name)s')
                        % {'done': done, 'total': total,
                           'name': name[-48:]}))
            except Exception as e:
                raise RuntimeError(
                    _('Overlay failed: ') + str(e))
            app.after(0, prog.set_stage_progress, 100.0)

            # ── Step 4: newfs ──
            app.after(0, prog.set_stage, 'init',
                _('Rebuilding .ffpkg with UFS2Tool newfs...'))
            app.after(0, app._fe_status_var.set,
                _('Rebuilding .ffpkg with UFS2Tool newfs...'))
            exe = _ufs2_exe(app)

            def _adv(attr, default):
                v = getattr(app, attr, None)
                if v is None:
                    return default
                try:
                    s = v.get() if hasattr(v, 'get') else v
                except Exception:
                    return default
                return (s or default)
            block = _adv('_adv_ffpkg_block_var',   '65536')
            frag  = _adv('_adv_ffpkg_frag_var',    '65536')
            minfr = _adv('_adv_ffpkg_minfree_var', '0')
            inode = _adv('_adv_ffpkg_inode_var',   '262144')
            try:
                if int(frag) > int(block):
                    frag = block
            except Exception:
                frag = block

            cmd = [exe, 'newfs',
                   '-O', '2',
                   '-b', block,
                   '-f', frag,
                   '-m', minfr,
                   '-S', '512',
                   '-i', inode,
                   '-D', dump_dir,
                   new_img]

            # Stream the newfs output so we can drive the progress bar
            # off UFS2Tool's own "%" reports rather than just guessing.
            try:
                p = subprocess.Popen(cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True, encoding='utf-8', errors='replace',
                    bufsize=1,
                    creationflags=_NO_WIN_FLAGS)
            except Exception as e:
                raise RuntimeError(_('newfs launch failed: ') + str(e))

            captured_tail = []
            newfs_stage = 'init'   # local — switches to 'files' when we
                                   # see "Adding files to image"
            try:
                for raw in iter(p.stdout.readline, ''):
                    line = raw.rstrip('\r\n')
                    if not line:
                        continue
                    # Keep a rolling tail for the failure message.
                    captured_tail.append(line)
                    if len(captured_tail) > 50:
                        del captured_tail[:-50]

                    stripped = line.strip()

                    # Stage transitions
                    if 'Writing cylinder groups' in stripped:
                        newfs_stage = 'init'
                        app.after(0, prog.set_stage, 'init',
                            _('Writing filesystem structure...'))
                    elif 'Adding files to image' in stripped:
                        newfs_stage = 'files'
                        app.after(0, prog.set_stage, 'files',
                            _('Copying files into image...'))
                    elif 'Populated image with' in stripped \
                            or 'Image created successfully' in stripped:
                        app.after(0, prog.set_stage_progress, 100.0,
                            _('Image created, finalising...'))

                    # Percentage parsing — uses the existing UFS2Tool
                    # output format "<stage>... NN% (...)".
                    import re as _re
                    m = _re.search(r'(\d{1,3})\s*%', stripped)
                    if not m:
                        continue
                    raw_pct = max(0, min(100, int(m.group(1))))

                    # File counts and byte progress
                    mf = _re.search(r'\((\d+)\s*/\s*(\d+)\s+files?',
                                     stripped)
                    files_done = files_total = 0
                    if mf:
                        files_done  = int(mf.group(1))
                        files_total = int(mf.group(2))
                    mg = _re.search(
                        r'([\d.]+)\s*GiB\s*/\s*([\d.]+)\s*GiB',
                        stripped)
                    written_gib = total_gib = 0.0
                    if mg:
                        written_gib = float(mg.group(1))
                        total_gib   = float(mg.group(2))

                    if newfs_stage == 'init':
                        app.after(0, prog.set_stage_progress,
                            float(raw_pct),
                            _('Initialising filesystem... %d%%') % raw_pct)
                    elif newfs_stage == 'files':
                        # Build the detail line from whatever fields we
                        # managed to parse.
                        bits = []
                        if files_total > 0:
                            bits.append('%d / %d files'
                                        % (files_done, files_total))
                        if total_gib > 0:
                            bits.append('%.2f GB / %.2f GB'
                                        % (written_gib, total_gib))
                        detail = '  •  '.join(bits) if bits \
                                                   else '%d%%' % raw_pct
                        app.after(0, prog.set_stage_progress,
                            float(raw_pct), detail)
            finally:
                # Drain any remaining output and wait.
                try:
                    rest = p.stdout.read()
                    if rest:
                        captured_tail.extend(rest.splitlines())
                except Exception:
                    pass
                try:
                    rc_newfs = p.wait(timeout=60)
                except Exception:
                    p.kill()
                    rc_newfs = -1

            if rc_newfs != 0:
                raise RuntimeError(
                    _('newfs failed: ') +
                    '\n'.join(captured_tail[-10:])[:800])
            if not os.path.isfile(new_img) or os.path.getsize(new_img) == 0:
                raise RuntimeError(
                    _('newfs reported success but the output file is '
                      'missing or empty.'))
            stats['rebuilt'] = True
            app.after(0, prog.set_stage_progress, 100.0,
                _('Image built'))

            # ── Step 5: atomic swap ──
            app.after(0, prog.set_stage, 'swap',
                _('Swapping in the rebuilt image...'))
            app.after(0, app._fe_status_var.set,
                _('Swapping in the rebuilt image...'))
            try:
                _bp_atomic_swap(new_img, img)
                stats['swapped'] = True
                app.after(0, prog.set_stage_progress, 100.0,
                    _('Done.'))
            except Exception as e:
                raise RuntimeError(
                    _('Could not swap in the rebuilt image: ') + str(e) +
                    '\n\n' +
                    _('The rebuilt image is at:\n') + new_img + '\n' +
                    _('Your original .ffpkg is unchanged and the backup '
                      'is at:\n') + backup_zip)

        except Exception as fatal_e:
            stats['errors'].append(str(fatal_e))

        finally:
            # Stop the poll thread.
            poll_state['stop'] = True
            # Keep the work dir if newfs succeeded but the swap failed
            # so the user can manually retrieve the rebuilt image.
            if not (stats['rebuilt'] and not stats['swapped']):
                _cleanup_work()
            try:
                cleanup_fn()
            except Exception:
                pass

        def _done():
            # Close the progress dialog first so the final messagebox
            # doesn't end up underneath it.
            try:
                prog.close()
            except Exception:
                pass
            _invalidate_cache(app, app._fe_current)
            _refresh(app)

            if stats['swapped']:
                app._fe_status_var.set(
                    _('Backport applied: ') +
                    _('rebuilt OK (%d files)') % stats['extracted_files'])
            else:
                app._fe_status_var.set(_('Backport failed.'))

            # Final dialog
            lines = []
            if stats['swapped']:
                lines.append(_('Backport applied via rebuild.'))
                lines.append('')
                lines.append(_('Files in rebuilt image: ') +
                             str(stats['extracted_files']))
                lines.append(_('Files newly added by backport: ') +
                             str(stats['overlay_added']))
                lines.append(_('Files replaced by backport: ') +
                             str(stats['overlay_replaced']))
                lines.append('')
                lines.append(_('Original .ffpkg saved as:'))
                lines.append(backup_zip)
                if stats['extract_errors']:
                    lines.append('')
                    lines.append(_('Note: %d extraction warning(s) — '
                                   'usually harmless but flagging in '
                                   'case the rebuild misses content.')
                                 % stats['extract_errors'])
                messagebox.showinfo(_('Backport applied'),
                    '\n'.join(lines))
            else:
                lines.append(_('Backport failed.'))
                lines.append('')
                lines.append(_('Your original .ffpkg is unchanged.'))
                lines.append(_('Backup of the original (just in case):'))
                lines.append(backup_zip)
                if stats['rebuilt'] and not stats['swapped']:
                    lines.append('')
                    lines.append(_('A rebuilt image was produced but '
                                   'could not be swapped into place. '
                                   'It is at:'))
                    lines.append(new_img)
                if stats['errors']:
                    lines.append('')
                    lines.append(_('First errors:'))
                    for e in stats['errors'][:5]:
                        lines.append('  - ' + e)
                    if len(stats['errors']) > 5:
                        lines.append('  ... (%d more)'
                                     % (len(stats['errors']) - 5))
                messagebox.showerror(_('Backport failed'),
                    '\n'.join(lines))

        app.after(0, _done)

    threading.Thread(target=_bg, daemon=True,
                     name='ffpkg-apply-backport').start()


def _scan_backport_source(src_dir):
    """Walk `src_dir` recursively, returning a sorted list of
    (relative_fs_path, absolute_source_path) tuples. Uses forward
    slashes for the relative path so it can be joined with the
    UFS2 fs path directly."""
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


def _ufs_file_exists(app, img, fs_path):
    """Return True if `fs_path` is a file inside `img`, False if it is
    not present (or is a directory), None if we couldn't tell.

    Strategy:
      - `stat` is the fastest and exact: if it returns rc=0, parse the
        first line; UFS2Tool reports 'directory' / 'regular file' / etc.
      - On any failure we fall back to listing the parent dir and
        checking whether the basename is present as a file.
    """
    rc, out = _run_ufs2(app, ['stat', img, fs_path], timeout=15)
    if rc == 0 and out:
        low = out.lower()
        if 'directory' in low:
            return False    # exists, but as a folder — caller treats as new
        if 'regular file' in low or 'file' in low:
            return True
    # Fallback — list parent and search.
    parent = _dirname_fs(fs_path)
    base   = _basename_fs(fs_path)
    rc, out = _run_ufs2(app, ['ls', img, parent], timeout=15)
    if rc != 0:
        return None
    for name, is_dir, _sz in _parse_ls(out):
        if name == base:
            return not is_dir
    return False


def _ufs_dir_exists(app, img, fs_path):
    """Return True if `fs_path` is a directory inside `img`, False
    otherwise. The root '/' is always treated as existing."""
    if fs_path in ('', '/'):
        return True
    rc, out = _run_ufs2(app, ['stat', img, fs_path], timeout=15)
    if rc == 0 and out and 'directory' in out.lower():
        return True
    # Fallback: ls the parent and look for an entry with matching name
    # marked as a directory.
    parent = _dirname_fs(fs_path)
    base   = _basename_fs(fs_path)
    rc, out = _run_ufs2(app, ['ls', img, parent], timeout=15)
    if rc != 0:
        return False
    for name, is_dir, _sz in _parse_ls(out):
        if name == base:
            return bool(is_dir)
    return False


# ─────────────────────────────────────────────────────────────────────
# Permission preservation (v2.0.6d)
#
# UFS2Tool's `add` verb gives newly-created files a default mode that
# doesn't always match what the PS5 expects. Anecdotally, applying a
# backport via `add`/`replace` produces ffpkgs that the game refuses
# to load even though packing the same files via `newfs -D` from a
# dump folder works fine. The most likely cause is mode bits (e.g.
# eboot.bin needing the execute bit, .sprx files needing read).
#
# Strategy:
#  - Before `replace`, capture the current mode of the destination.
#    After `replace`, chmod the new file back to the captured mode.
#    This means a replaced file always ends up with the exact mode
#    the original had — guaranteed safe.
#  - For per-file `add`, there's no "previous mode" to copy. Pick a
#    sensible default: 0755 if the name looks like an ELF executable,
#    0644 otherwise. (Heuristic only — see _default_mode_for_name.)
#  - For bulk-add of a whole subtree, use the recursive chmod form
#    `chmod -R <img> 644 755` (the two-argument form sets file-mode
#    and dir-mode separately), then a one-off chmod 755 on any
#    executable-looking files we find.
# ─────────────────────────────────────────────────────────────────────
def _ufs_get_mode(app, img, fs_path):
    """Return the octal mode (int, e.g. 0o644) of `fs_path` inside
    `img`, or None if it can't be determined. Parses the `stat`
    output's first numeric octal value that looks like a Unix mode.
    """
    rc, out = _run_ufs2(app, ['stat', img, fs_path], timeout=15)
    if rc != 0 or not out:
        return None
    # The output contains lines like:
    #   Permissions: 0644 (-rw-r--r--)
    # or some variant. Match an explicit "0NNNN" octal first; otherwise
    # any 3-4 digit octal preceded by "perm"/"mode"/"access".
    import re as _re
    # Pass 1: explicit zero-prefixed octal anywhere in the output.
    m = _re.search(r'\b0[0-7]{3,4}\b', out)
    if m:
        try:
            return int(m.group(0), 8)
        except Exception:
            pass
    # Pass 2: 3-4 digit octal adjacent to a permissions-ish word, with
    # arbitrary non-digit characters in between (commonly ': ').
    m = _re.search(r'(?i)(?:perm|mode|access)\D{0,16}([0-7]{3,4})\b', out)
    if m:
        try:
            return int(m.group(1), 8)
        except Exception:
            pass
    return None


def _ufs_set_mode(app, img, fs_path, mode):
    """Set `fs_path`'s mode inside `img`. `mode` is an int like 0o644.
    Returns True on success.
    """
    mode_str = '%o' % (mode & 0o7777)
    rc, _out = _run_ufs2(app, ['chmod', img, mode_str, fs_path],
                          timeout=30)
    return rc == 0


def _ufs_chmod_recursive(app, img, fs_path, file_mode, dir_mode):
    """Recursively chmod everything under `fs_path` to file_mode/dir_mode.
    Uses UFS2Tool's two-argument recursive form:
        chmod -R <img> <file_mode> <dir_mode> <fs_path>
    Returns True on success.
    """
    fmode = '%o' % (file_mode & 0o7777)
    dmode = '%o' % (dir_mode  & 0o7777)
    # Per the UFS2Tool README: `chmod -R <image> <file_mode> <dir_mode>`
    # applies recursively from root. The README's separately-shown
    # single-path form is `chmod <image> <mode> <fs_path>`; combining
    # the two (recursive on a specific subtree) is the natural extension
    # and UFS2Tool accepts it. If a build rejects the combined form,
    # we fall back to a manual walk via ls+chmod.
    rc, _out = _run_ufs2(app, ['chmod', '-R', img, fmode, dmode, fs_path],
                          timeout=300)
    if rc == 0:
        return True
    # Fallback: walk and chmod individually.
    return _ufs_chmod_walk(app, img, fs_path, file_mode, dir_mode)


def _ufs_chmod_walk(app, img, fs_path, file_mode, dir_mode, _depth=0):
    """Manual recursive chmod fallback for when the bulk form fails."""
    if _depth > 32:
        return False
    # Set the directory's own mode first.
    _ufs_set_mode(app, img, fs_path, dir_mode)
    rc, out = _run_ufs2(app, ['ls', img, fs_path], timeout=60)
    if rc != 0:
        return False
    ok = True
    for name, is_dir, _sz in _parse_ls(out):
        if name in ('.', '..'):
            continue
        child = _join_fs_path(fs_path, name)
        if is_dir:
            if not _ufs_chmod_walk(app, img, child, file_mode, dir_mode,
                                    _depth + 1):
                ok = False
        else:
            if not _ufs_set_mode(app, img, child, file_mode):
                ok = False
    return ok


def _default_mode_for_name(name):
    """Heuristic default mode for a freshly-added file when we have no
    prior file to copy from. 0755 for things that look like executables,
    0644 for everything else. Permissions on PS5 are surprisingly
    forgiving for data files but the loader insists on the execute bit
    for the main ELF, so this is the bit we try not to get wrong.
    """
    lname = name.lower()
    if lname == 'eboot.bin':
        return 0o755
    if lname.endswith('.elf'):
        return 0o755
    return 0o644


def _bulk_add_new_subtrees(app, img, items, base_fs_dir, src_root, stats):
    """Handle fresh top-level subdirectories with one `add` call each.

    UFS2Tool's `add` verb refuses to create parent directories on its
    own — it errors with "Path component X not found in inode N" for
    any path whose parent doesn't already exist in the image. The
    placeholder-file trick doesn't get around this because `add` fails
    on the placeholder for the same reason.

    The one operation UFS2Tool DOES support for creating directories
    is `add <img> <fs_dir> <local_dir>` with a directory source — that
    recursively creates the destination directory and copies its
    contents.

    So: for every first-level directory key in `items` that doesn't
    yet exist inside `base_fs_dir`, we issue one bulk `add` with the
    matching on-disk directory as the source. UFS2Tool creates the
    whole subtree in one go.

    Returns a set of relative item paths that were handled here so the
    per-file pass can skip them (counts them as 'added' in stats).
    """
    handled = set()

    # Group items by first-level directory key.
    # 'fakelib/foo.sprx' → 'fakelib'
    # 'sce_sys/x/y.bin' → 'sce_sys'
    # 'eboot.bin'       → '' (root file — never bulk-added)
    groups = {}
    for rel, _src in items:
        key = rel.split('/', 1)[0] if '/' in rel else ''
        if not key:
            continue   # root-level files are always per-file
        groups.setdefault(key, []).append(rel)

    for key, rels in groups.items():
        dst_fs = _join_fs_path(base_fs_dir, key)
        if _ufs_dir_exists(app, img, dst_fs):
            # Directory already exists — fall through to per-file pass
            # so we get proper file-level replace + backup semantics.
            continue
        src_local = os.path.join(src_root, key)
        if not os.path.isdir(src_local):
            # Shouldn't happen (we scanned this dir), but guard anyway.
            continue
        rc, out = _run_ufs2(app, ['add', img, dst_fs, src_local],
                             timeout=600)
        if rc != 0:
            stats['errors'].append(
                _('bulk add failed %s: ') % dst_fs + out.strip()[:200])
            # Don't mark these as handled — let the per-file pass try
            # them individually (it'll likely also fail, but at least
            # each failure gets reported precisely).
            continue
        # Success: count one bulk-add as N file-adds and mark every
        # rel under this key as handled.
        stats['added'] += len(rels)
        handled.update(rels)

        # Fix permissions on the freshly-added subtree. UFS2Tool's
        # `add` gives newly-created entries a default mode that may
        # not be what the PS5 expects (e.g. .sprx files unreadable,
        # eboot.bin non-executable). Use the two-mode recursive chmod
        # to set files to 0644 and directories to 0755 in one call.
        _ufs_chmod_recursive(app, img, dst_fs, 0o644, 0o755)

        # Any ELF-looking files inside the new subtree need the
        # execute bit. Scan the on-disk source so we don't need to
        # round-trip the image again.
        try:
            for root, _dirs, files in os.walk(src_local):
                for fn in files:
                    lfn = fn.lower()
                    if lfn == 'eboot.bin' or lfn.endswith('.elf'):
                        try:
                            ap     = os.path.join(root, fn)
                            rel_in = os.path.relpath(ap, src_local) \
                                        .replace(os.sep, '/')
                            elf_fs = _join_fs_path(dst_fs, rel_in)
                            _ufs_set_mode(app, img, elf_fs, 0o755)
                        except Exception:
                            pass
        except Exception:
            pass

    return handled


def _extract_to_backup(app, img, fs_path, backup_dir, rel):
    """UFS2Tool extract `fs_path` from `img` into a temp dir, then move
    the extracted file to <backup_dir>/<rel>. Returns (ok, err_msg)."""
    tmp = tempfile.mkdtemp(prefix='ffpkg_bkp_')
    try:
        rc, out = _run_ufs2(app, ['extract', img, tmp, fs_path],
                            timeout=120)
        if rc != 0:
            return False, out.strip()[:300]
        # Find the extracted file. UFS2Tool preserves the fs path
        # structure under tmp, but be defensive — also search by name.
        target_name = _basename_fs(fs_path)
        found = None
        for root, _dirs, files in os.walk(tmp):
            for f in files:
                if f == target_name:
                    found = os.path.join(root, f)
                    break
            if found:
                break
        if not found:
            return False, _('extracted file not found in temp dir')
        dst = os.path.join(backup_dir, rel.replace('/', os.sep))
        try:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
        except Exception as e:
            return False, str(e)
        try:
            # shutil.move would do a cross-device rename; for safety
            # just read+write so the temp dir can be cleaned up cleanly.
            with open(found, 'rb') as fh_r, open(dst, 'wb') as fh_w:
                while True:
                    chunk = fh_r.read(1 << 20)
                    if not chunk:
                        break
                    fh_w.write(chunk)
        except Exception as e:
            return False, str(e)
        return True, ''
    finally:
        # Best-effort cleanup of the temp dir.
        try:
            import shutil as _sh
            _sh.rmtree(tmp, ignore_errors=True)
        except Exception:
            pass


def _write_backup_manifest(backup_dir, img, src_dir, base_fs_dir,
                           ts, stats):
    """Write a backup_manifest.txt at backup_dir/ root with enough
    info to restore the originals if needed."""
    lines = []
    lines.append('exFAT Image Builder — backport backup manifest')
    lines.append('=' * 50)
    lines.append('Timestamp:        ' + ts)
    lines.append('Image:            ' + img)
    lines.append('Backport source:  ' + src_dir)
    lines.append('Anchored at:      ' + (base_fs_dir or '/'))
    lines.append('Replaced count:   ' + str(stats['replaced']))
    lines.append('Added count:      ' + str(stats['added']))
    lines.append('Failed count:     ' + str(stats['failed']))
    lines.append('')
    lines.append('Files backed up (originals from the image, '
                 'extracted BEFORE replace):')
    for p in stats['backed_up']:
        lines.append('  ' + p)
    if stats['errors']:
        lines.append('')
        lines.append('Errors:')
        for e in stats['errors']:
            lines.append('  - ' + e)
    lines.append('')
    lines.append('To restore: open this image in Edit ffpkg, navigate to '
                 'the same anchor folder, and use Add folder on the '
                 'extracted backup folder. Replaced files will be put '
                 'back in place (additive merge — non-destructive).')
    try:
        with open(os.path.join(backup_dir, 'backup_manifest.txt'),
                  'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines))
    except Exception:
        pass  # Manifest is best-effort — don't fail the whole op.


def _zip_backup_folder(backup_dir, backup_zip):
    """Zip the backup folder. Removes the folder on success so the
    zip is the canonical artifact."""
    with _backport_zipfile.ZipFile(backup_zip, 'w',
            _backport_zipfile.ZIP_DEFLATED) as zf:
        for root, _dirs, files in os.walk(backup_dir):
            for f in files:
                ap = os.path.join(root, f)
                rel = os.path.relpath(ap, backup_dir)
                zf.write(ap, rel)
    # Delete the now-redundant folder.
    try:
        import shutil as _sh
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
    from tkinter import Toplevel
    result = {'kind': None}

    dlg = Toplevel(app)
    dlg.title(_('Apply backport — pick source'))
    dlg.configure(bg=COLORS['bg_1'])
    dlg.transient(app)
    dlg.grab_set()
    dlg.resizable(False, False)

    dlg_w, dlg_h = 480, 320
    dlg.update_idletasks()
    try:
        x = app.winfo_x() + (app.winfo_width()  - dlg_w) // 2
        y = app.winfo_y() + (app.winfo_height() - dlg_h) // 2
        dlg.geometry('%dx%d+%d+%d' % (dlg_w, dlg_h, x, y))
    except Exception:
        dlg.geometry('%dx%d' % (dlg_w, dlg_h))

    body = tk.Frame(dlg, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, padx=18, pady=16)

    tk.Label(body, text=_('What\u2019s your backport source?'),
             font=('Segoe UI', 12, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_1']
             ).pack(anchor='w')
    tk.Label(body,
             text=_('Pick the format that matches what you have.'),
             font=('Segoe UI', 9),
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(anchor='w', pady=(2, 14))

    def _pick(k):
        result['kind'] = k
        dlg.destroy()

    def _row(icon, title, sub, kind):
        outer = tk.Frame(body, bg=COLORS['bg_2'],
                         highlightbackground=COLORS['border_3'],
                         highlightthickness=1, cursor='hand2')
        outer.pack(fill='x', pady=(0, 6))
        # Bind whole row to pick
        def _on_click(_e=None, k=kind): _pick(k)
        outer.bind('<Button-1>', _on_click)

        inner = tk.Frame(outer, bg=COLORS['bg_2'])
        inner.pack(fill='x', padx=12, pady=10)
        inner.bind('<Button-1>', _on_click)
        ico = tk.Label(inner, text=icon, font=('Segoe UI', 18),
                       bg=COLORS['bg_2'], fg=COLORS['accent'])
        ico.pack(side='left', padx=(0, 12))
        ico.bind('<Button-1>', _on_click)
        col = tk.Frame(inner, bg=COLORS['bg_2'])
        col.pack(side='left', fill='x', expand=True)
        col.bind('<Button-1>', _on_click)
        tlbl = tk.Label(col, text=title,
                        font=('Segoe UI', 10, 'bold'),
                        bg=COLORS['bg_2'], fg=COLORS['fg_1'])
        tlbl.pack(anchor='w')
        tlbl.bind('<Button-1>', _on_click)
        slbl = tk.Label(col, text=sub,
                        font=('Segoe UI', 8),
                        bg=COLORS['bg_2'], fg=COLORS['fg_4'])
        slbl.pack(anchor='w')
        slbl.bind('<Button-1>', _on_click)

    _row('\U0001f4c1', _('A folder'),
         _('A backport folder like the ones produced by Auto Backport '
           '(e.g. fakelib/, sce_module/, sce_sys/, eboot.bin).'),
         'folder')
    _row('\U0001f4e6', _('A .ffpkg image'),
         _('Read the backport tree out of a .ffpkg using UFS2Tool, '
           'then overlay it onto the open image.'),
         'ffpkg')
    _row('\U0001f4be', _('An exFAT image'),
         _('Mount a .exfat / .img read-only via OSFMount, copy its '
           'contents to a temp folder, then overlay.'),
         'exfat')

    # Cancel button
    btnbar = tk.Frame(body, bg=COLORS['bg_1'])
    btnbar.pack(fill='x', pady=(8, 0))
    _ghost_btn(btnbar, _('Cancel'), command=dlg.destroy
               ).pack(side='right')

    app.wait_window(dlg)
    return result['kind']


# ─────────────────────────────────────────────────────────────────────
# Source materialisation
# ─────────────────────────────────────────────────────────────────────
def _bp_materialize_source(app, kind, src_path):
    """Turn the chosen source into a real folder on disk.

    Returns (src_dir, cleanup_fn). cleanup_fn is always callable.
    For kind='folder', cleanup_fn is a no-op and src_dir == src_path.
    For kind='ffpkg' and kind='exfat', src_dir is a temp dir under
    %TEMP% and cleanup_fn removes it.
    """
    if kind == 'folder':
        return src_path, (lambda: None)

    if kind == 'ffpkg':
        return _bp_extract_ffpkg_to_temp(app, src_path)

    if kind == 'exfat':
        return _bp_extract_exfat_to_temp(app, src_path)

    raise ValueError('unknown kind: ' + repr(kind))


def _bp_extract_ffpkg_to_temp(app, ffpkg_path):
    """UFS2Tool extract the whole tree from `ffpkg_path` into a temp
    dir. Returns (temp_dir, cleanup_fn)."""
    import shutil as _sh
    if not os.path.isfile(ffpkg_path):
        raise RuntimeError(_('Source .ffpkg does not exist: ') + ffpkg_path)
    tmp = tempfile.mkdtemp(prefix='ffpkg_bp_src_')
    def _cleanup():
        try:
            _sh.rmtree(tmp, ignore_errors=True)
        except Exception:
            pass
    try:
        # UFS2Tool: `extract <img> <out_dir> <fs_path>`. Passing '/'
        # extracts the root, but UFS2Tool's extract verb requires a
        # specific file path. So we do a recursive walk via `ls` and
        # extract each file individually. That's also how the existing
        # hex-preview path uses it.
        stats = {'files': 0, 'errors': []}
        _ffpkg_extract_walk(app, ffpkg_path, '/', tmp, stats)
        if not stats['files']:
            raise RuntimeError(
                _('No files were extracted from the source .ffpkg.'))
        return tmp, _cleanup
    except Exception:
        _cleanup()
        raise


def _ffpkg_extract_walk(app, img, fs_dir, local_dir, stats, _depth=0):
    """Recursive helper. Walks the fs tree under `fs_dir` and extracts
    every regular file under `local_dir`, preserving the path. Skips
    entries we can't classify."""
    if _depth > 32:
        stats['errors'].append(_('recursion too deep at ') + fs_dir)
        return
    rc, out = _run_ufs2(app, ['ls', img, fs_dir], timeout=60)
    if rc != 0:
        stats['errors'].append('ls %s: %s' % (fs_dir, out.strip()[:200]))
        return
    parsed = _parse_ls(out)
    for name, is_dir, _sz in parsed:
        if name in ('.', '..'):
            continue
        child_fs = _join_fs_path(fs_dir, name)
        if is_dir:
            # Recurse — UFS2Tool will need this path to ls.
            _ffpkg_extract_walk(app, img, child_fs, local_dir, stats,
                                _depth + 1)
        else:
            # Extract this one file.
            rc, out = _run_ufs2(app, ['extract', img, local_dir, child_fs],
                                 timeout=300)
            if rc != 0:
                stats['errors'].append('extract %s: %s'
                                        % (child_fs, out.strip()[:200]))
                continue
            stats['files'] += 1


def _bp_extract_exfat_to_temp(app, img_path):
    """Mount `img_path` read-only via OSFMount, copy its tree into a
    temp dir, then dismount. Returns (temp_dir, cleanup_fn).
    """
    import shutil as _sh
    import ctypes as _ct
    if not os.path.isfile(img_path):
        raise RuntimeError(_('Source exFAT image does not exist: ') + img_path)

    osf = None
    try:
        osf = app._find_osfmount()
    except Exception:
        osf = None
    if not osf:
        raise RuntimeError(_(
            'OSFMount is required to read exFAT images but was not '
            'found. Install it (or set its path in Settings) and try '
            'again.'))

    tmp = tempfile.mkdtemp(prefix='exfat_bp_src_')
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
        # Find a free drive letter.
        drives_bitmask = _ct.windll.kernel32.GetLogicalDrives()
        free_letter = None
        for i in range(25, 3, -1):
            if not (drives_bitmask & (1 << i)):
                free_letter = chr(65 + i) + ':'
                break
        if not free_letter:
            raise RuntimeError(_('No free drive letters available for mount.'))

        # Mount read-only + removable.
        result = subprocess.run(
            [osf, '-a', '-t', 'file', '-f', img_path,
             '-m', free_letter, '-o', 'ro,rem'],
            capture_output=True, text=True, timeout=120,
            creationflags=_NO_WIN_FLAGS)
        if result.returncode != 0:
            raise RuntimeError(_('OSFMount mount failed: ') +
                (result.stderr or result.stdout or '')[:300])
        mounted['drive'] = free_letter

        # Wait for the drive to appear.
        import time as _t
        for _ in range(30):
            if os.path.exists(free_letter + '\\'):
                break
            _t.sleep(0.5)
        else:
            raise RuntimeError(_('Mounted drive did not appear in time.'))

        # Copy the tree into tmp. We use os.walk + shutil.copy2 rather
        # than robocopy so we don't depend on robocopy's exit codes
        # (which use bitfields and confuse simple rc checks).
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
                    import shutil as _sh2
                    _sh2.copy2(src, dst)
                except Exception:
                    # Skip files we can't read; the user will see the
                    # final manifest count and can investigate.
                    pass

        # Dismount eagerly now that the copy is done — don't keep the
        # drive busy during the overlay.
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


# ─────────────────────────────────────────────────────────────────────
# Rebuild-flow helpers (v2.0.6e)
# ─────────────────────────────────────────────────────────────────────
def _bp_free_space(path):
    """Return bytes free on the filesystem holding `path`, or None if
    it can't be determined."""
    try:
        import shutil as _sh
        return _sh.disk_usage(path).free
    except Exception:
        try:
            st = os.statvfs(path)
            return st.f_bavail * st.f_frsize
        except Exception:
            return None


def _bp_fmt_size(n):
    """Format a byte count as e.g. '12.4 GB' or '847 MB'."""
    if n is None:
        return '?'
    n = float(n)
    for unit in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1024.0 or unit == 'TB':
            return '%.1f %s' % (n, unit) if unit != 'B' else '%d B' % n
        n /= 1024.0


def _bp_zip_single_file(src, dst_zip, progress_cb=None):
    """Zip a single file `src` into `dst_zip`. The archive contains
    just one entry, named the same as `src`'s basename, so restoring
    is trivial: unzip and you have the original file back.

    Optional `progress_cb(done_bytes, total_bytes)` is called as the
    file is streamed in, roughly every 8 MB. Used by the progress
    dialog so the backup step shows live percentage.
    """
    import zipfile as _zf
    if os.path.exists(dst_zip):
        try:
            os.unlink(dst_zip)
        except Exception:
            pass
    try:
        total = os.path.getsize(src)
    except Exception:
        total = 0

    with _zf.ZipFile(dst_zip, 'w', _zf.ZIP_DEFLATED, allowZip64=True) as zf:
        if progress_cb is None or total <= 0:
            # Simple path — no progress tracking.
            zf.write(src, arcname=os.path.basename(src))
            return
        # Streamed path — write in chunks so we can report progress.
        arcname = os.path.basename(src)
        try:
            with zf.open(arcname, 'w', force_zip64=True) as zinfo, \
                 open(src, 'rb') as fh:
                done = 0
                last_report = 0
                chunk_size = 1 << 23   # 8 MB
                while True:
                    chunk = fh.read(chunk_size)
                    if not chunk:
                        break
                    zinfo.write(chunk)
                    done += len(chunk)
                    if done - last_report >= chunk_size or done == total:
                        try:
                            progress_cb(done, total)
                        except Exception:
                            pass
                        last_report = done
        except Exception:
            # Fallback to the simple path if streamed write fails for
            # any reason (older zipfile builds, weird path issues).
            zf.write(src, arcname=arcname)


def _bp_overlay_into_dump(items, anchor_abs, stats, progress_cb=None):
    """Copy every (rel, abs_src) from `items` into `anchor_abs`,
    preserving relative path. Existing files at the destination are
    counted as 'overlay_replaced', new ones as 'overlay_added'.

    `anchor_abs` is the on-disk directory we're overlaying onto
    (typically <dump_dir> for breadcrumb '/').

    Optional `progress_cb(done, total, current_name)` is called once
    per file processed.

    Failures are not fatal — they get recorded in stats['errors'] and
    the rebuild proceeds with whatever made it onto disk.
    """
    import shutil as _sh
    if not os.path.isdir(anchor_abs):
        try:
            os.makedirs(anchor_abs, exist_ok=True)
        except Exception as e:
            raise RuntimeError(
                _('Could not create overlay anchor dir %s: %s')
                % (anchor_abs, e))

    total = len(items)
    for i, (rel, abs_src) in enumerate(items, 1):
        dst = os.path.join(anchor_abs, rel.replace('/', os.sep))
        existed = os.path.isfile(dst)
        try:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            _sh.copy2(abs_src, dst)
            if existed:
                stats['overlay_replaced'] += 1
            else:
                stats['overlay_added'] += 1
        except Exception as e:
            stats['errors'].append(
                _('overlay copy failed %s: ') % dst + str(e)[:200])
        if progress_cb is not None:
            try:
                progress_cb(i, total, rel)
            except Exception:
                pass


def _bp_atomic_swap(new_path, target_path):
    """Replace `target_path` with `new_path` as atomically as the
    filesystem allows.

    Strategy on Windows: os.replace() is atomic when source and
    destination are on the same volume — which they always are here,
    because new_path is in a temp dir we deliberately keep next to
    target. If new_path is on a different volume (rare — happens only
    if TEMP is on a different drive than the image), fall back to a
    copy+rename: copy to <target>.new, fsync, then os.replace.
    """
    same_drive = (os.path.splitdrive(os.path.abspath(new_path))[0].lower()
                  == os.path.splitdrive(os.path.abspath(target_path))[0].lower())
    if same_drive:
        os.replace(new_path, target_path)
        return

    # Cross-volume: copy through a sidecar on the destination volume,
    # then rename over the original.
    import shutil as _sh
    sidecar = target_path + '.new'
    try:
        if os.path.exists(sidecar):
            os.unlink(sidecar)
    except Exception:
        pass
    _sh.copy2(new_path, sidecar)
    # Force buffers to disk before the rename so a power loss between
    # the two ops doesn't leave a half-written sidecar that then gets
    # renamed over the original. Not bulletproof — Windows doesn't
    # offer a real fsync — but better than nothing.
    try:
        with open(sidecar, 'rb') as fh:
            os.fsync(fh.fileno())
    except Exception:
        pass
    os.replace(sidecar, target_path)
    try:
        os.unlink(new_path)
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────
# Compare ffpkg (v2.0.6g) — diagnostic tool that extracts two ffpkgs
# and diffs them, so we can find out why a rebuilt image differs from
# a manually-built one.
#
# The report includes:
#   - files only in A vs only in B
#   - files present in both but with different size or SHA256
#   - mode (chmod) differences per file (via UFS2Tool stat)
#   - BSD inode-flag differences per file if UFS2Tool's stat exposes them
#   - top-level directory entry-count differences
#
# Output: written to a .compare.txt file next to ffpkg A.
# ─────────────────────────────────────────────────────────────────────
def _compare_ffpkgs(app):
    """Toolbar entry-point. Asks the user to pick two .ffpkg files,
    extracts both, and writes a diff report to disk.
    """
    a = filedialog.askopenfilename(
        title=_('Pick first .ffpkg (A — usually the good / working one)'),
        filetypes=[('FFPKG image', '*.ffpkg'),
                   ('All files',   '*.*')])
    if not a:
        return
    b = filedialog.askopenfilename(
        title=_('Pick second .ffpkg (B — usually the broken one)'),
        filetypes=[('FFPKG image', '*.ffpkg'),
                   ('All files',   '*.*')])
    if not b:
        return
    if os.path.abspath(a) == os.path.abspath(b):
        messagebox.showerror(_('Same file'),
            _('Both picks are the same file. Choose two different .ffpkgs.'))
        return

    if not messagebox.askyesno(_('Compare ffpkgs'),
        _('This will extract BOTH images to temp folders and walk every '
          'file in each, comparing sizes, hashes, and modes. It can take '
          'a while for large games (several minutes).\n\n') +
        _('A:  ') + a + '\n' +
        _('B:  ') + b + '\n\n' +
        _('Output report will be written next to A.\n\n') +
        _('Proceed?')):
        return

    app._fe_status_var.set(_('Extracting A...'))
    app.update_idletasks()

    def _bg():
        report_path = os.path.splitext(a)[0] + '.compare.txt'
        work_dir = tempfile.mkdtemp(prefix='ffpkg_compare_')
        dump_a   = os.path.join(work_dir, 'A')
        dump_b   = os.path.join(work_dir, 'B')
        try:
            os.makedirs(dump_a, exist_ok=True)
            os.makedirs(dump_b, exist_ok=True)

            app.after(0, app._fe_status_var.set, _('Extracting A...'))
            rc, out = _run_ufs2(app, ['extract', a, dump_a], timeout=3600)
            if rc != 0:
                rc, out = _run_ufs2(app,
                    ['extract', a, dump_a, '/'], timeout=3600)
            if rc != 0:
                raise RuntimeError(_('Extract A failed: ') +
                                   (out or '').strip()[:300])

            app.after(0, app._fe_status_var.set, _('Extracting B...'))
            rc, out = _run_ufs2(app, ['extract', b, dump_b], timeout=3600)
            if rc != 0:
                rc, out = _run_ufs2(app,
                    ['extract', b, dump_b, '/'], timeout=3600)
            if rc != 0:
                raise RuntimeError(_('Extract B failed: ') +
                                   (out or '').strip()[:300])

            # ── Collect inventories ──
            app.after(0, app._fe_status_var.set, _('Scanning A...'))
            inv_a = _ffpkg_scan_dump(dump_a)
            app.after(0, app._fe_status_var.set, _('Scanning B...'))
            inv_b = _ffpkg_scan_dump(dump_b)

            # ── Collect UFS2-side metadata (modes, flags) ──
            app.after(0, app._fe_status_var.set,
                _('Reading metadata from A...'))
            meta_a = _ffpkg_collect_stat_meta(app, a,
                set(inv_a['files']) | set(inv_a['dirs']))
            app.after(0, app._fe_status_var.set,
                _('Reading metadata from B...'))
            meta_b = _ffpkg_collect_stat_meta(app, b,
                set(inv_b['files']) | set(inv_b['dirs']))

            # ── Diff ──
            app.after(0, app._fe_status_var.set, _('Writing report...'))
            _ffpkg_write_compare_report(
                report_path, a, b,
                inv_a, inv_b, meta_a, meta_b, dump_a, dump_b)

            app.after(0, app._fe_status_var.set,
                _('Compare report saved.'))
            app.after(0, lambda: messagebox.showinfo(
                _('Compare done'),
                _('Report written to:\n') + report_path + '\n\n' +
                _('Open it and look for sections marked "ONLY IN A", '
                  '"ONLY IN B", "DIFFERENT SIZE", "DIFFERENT HASH", and '
                  '"DIFFERENT MODE". Anything in those sections is a '
                  'concrete difference between the two images.')))
        except Exception as e:
            app.after(0, app._fe_status_var.set,
                _('Compare failed.'))
            app.after(0, lambda e=e: messagebox.showerror(
                _('Compare failed'), str(e)))
        finally:
            try:
                import shutil as _sh
                _sh.rmtree(work_dir, ignore_errors=True)
            except Exception:
                pass

    threading.Thread(target=_bg, daemon=True,
                     name='ffpkg-compare').start()


def _ffpkg_scan_dump(dump_root):
    """Walk an extracted dump, returning:
        {
          'files': {rel_path: size_bytes, ...},
          'dirs':  set of rel_paths,
          'sha':   {rel_path: hex_sha256, ...},      # filled lazily
        }
    rel_paths are forward-slash, no leading slash.
    """
    import hashlib as _hashlib
    inv = {'files': {}, 'dirs': set(), 'sha': {}}
    dump_root = os.path.abspath(dump_root)
    for root, dirs, files in os.walk(dump_root):
        for d in dirs:
            ap = os.path.join(root, d)
            try:
                rel = os.path.relpath(ap, dump_root).replace(os.sep, '/')
            except Exception:
                continue
            inv['dirs'].add(rel)
        for f in files:
            ap = os.path.join(root, f)
            try:
                rel = os.path.relpath(ap, dump_root).replace(os.sep, '/')
            except Exception:
                continue
            try:
                inv['files'][rel] = os.path.getsize(ap)
            except Exception:
                inv['files'][rel] = -1
    return inv


def _ffpkg_sha256(path):
    """Return hex SHA-256 of a file, or None on error."""
    import hashlib as _hashlib
    h = _hashlib.sha256()
    try:
        with open(path, 'rb') as fh:
            while True:
                chunk = fh.read(1 << 20)
                if not chunk:
                    break
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return None


def _ffpkg_collect_stat_meta(app, img, rel_paths):
    """Run UFS2Tool `stat` on each path inside `img` and parse out the
    mode (octal int) and BSD inode flags (hex int) if present.
    Returns: {rel_path: {'mode': int|None, 'flags': int|None,
                         'raw': str}}.

    `rel_paths` are forward-slash without leading slash (matching
    `_ffpkg_scan_dump`). Capped at 10000 entries to keep this from
    spending forever on very large images.
    """
    import re as _re
    meta = {}
    count = 0
    cap = 10000
    for rel in sorted(rel_paths):
        if count >= cap:
            meta['__truncated__'] = {'mode': None, 'flags': None,
                'raw': 'metadata read capped at %d entries' % cap}
            break
        count += 1
        fs_path = '/' + rel if rel else '/'
        rc, out = _run_ufs2(app, ['stat', img, fs_path], timeout=15)
        if rc != 0 or not out:
            meta[rel] = {'mode': None, 'flags': None,
                'raw': (out or '').strip()[:200]}
            continue
        mode = None
        m = _re.search(r'\b0[0-7]{3,4}\b', out)
        if m:
            try:
                mode = int(m.group(0), 8)
            except Exception:
                mode = None
        if mode is None:
            m = _re.search(r'(?i)(?:perm|mode|access)\D{0,16}([0-7]{3,4})\b', out)
            if m:
                try:
                    mode = int(m.group(1), 8)
                except Exception:
                    mode = None
        # Look for a "flags" line — UFS2Tool stat output sometimes
        # includes something like "Flags: 0x00000010" or
        # "Inode flags: 0".
        flags = None
        m = _re.search(r'(?i)\bflags\b\s*[:=]?\s*(0x[0-9a-f]+|\d+)', out)
        if m:
            raw = m.group(1)
            try:
                flags = int(raw, 16) if raw.lower().startswith('0x') \
                                     else int(raw)
            except Exception:
                flags = None
        meta[rel] = {'mode': mode, 'flags': flags, 'raw': out.strip()}
    return meta


def _ffpkg_write_compare_report(path, a_img, b_img,
                                 inv_a, inv_b, meta_a, meta_b,
                                 dump_a, dump_b):
    """Write a human-readable diff report comparing two extracted
    ffpkg dumps + their UFS2 metadata. SHA-256s are computed on
    demand only for files that exist in both and have the same size
    (cheap-first; mismatched sizes are already a known-diff)."""
    import hashlib as _hashlib

    files_a = inv_a['files']
    files_b = inv_b['files']
    dirs_a  = inv_a['dirs']
    dirs_b  = inv_b['dirs']

    only_a_files = sorted(set(files_a) - set(files_b))
    only_b_files = sorted(set(files_b) - set(files_a))
    only_a_dirs  = sorted(dirs_a - dirs_b)
    only_b_dirs  = sorted(dirs_b - dirs_a)

    common_files = sorted(set(files_a) & set(files_b))

    size_diff   = []
    hash_diff   = []
    mode_diff   = []
    flag_diff   = []

    for rel in common_files:
        sa, sb = files_a[rel], files_b[rel]
        if sa != sb:
            size_diff.append((rel, sa, sb))
            continue
        # Same size — check SHA256.
        pa = os.path.join(dump_a, rel.replace('/', os.sep))
        pb = os.path.join(dump_b, rel.replace('/', os.sep))
        ha = _ffpkg_sha256(pa)
        hb = _ffpkg_sha256(pb)
        if ha is None or hb is None:
            continue
        if ha != hb:
            hash_diff.append((rel, ha, hb))

    # Mode + flag comparison from metadata maps (includes both files
    # and dirs; only files that exist on both sides though).
    common_paths = (set(files_a) & set(files_b)) | (dirs_a & dirs_b)
    for rel in sorted(common_paths):
        ma = meta_a.get(rel, {})
        mb = meta_b.get(rel, {})
        m_a, m_b = ma.get('mode'), mb.get('mode')
        if m_a is not None and m_b is not None and m_a != m_b:
            mode_diff.append((rel, m_a, m_b))
        f_a, f_b = ma.get('flags'), mb.get('flags')
        if f_a is not None and f_b is not None and f_a != f_b:
            flag_diff.append((rel, f_a, f_b))

    # ── Write the report ──
    lines = []
    lines.append('FFPKG COMPARE REPORT')
    lines.append('=' * 72)
    lines.append('A:  ' + a_img)
    lines.append('B:  ' + b_img)
    lines.append('')
    lines.append('Image sizes (bytes on disk):')
    try:
        lines.append('  A:  %d' % os.path.getsize(a_img))
    except Exception:
        lines.append('  A:  ?')
    try:
        lines.append('  B:  %d' % os.path.getsize(b_img))
    except Exception:
        lines.append('  B:  ?')
    lines.append('')
    lines.append('Entry counts:')
    lines.append('  A:  %d files, %d dirs' % (len(files_a), len(dirs_a)))
    lines.append('  B:  %d files, %d dirs' % (len(files_b), len(dirs_b)))
    lines.append('')

    def _sec(title, items, fmt):
        lines.append('-' * 72)
        lines.append('%s  (%d)' % (title, len(items)))
        if not items:
            lines.append('  (none)')
        else:
            for item in items[:500]:
                lines.append('  ' + fmt(item))
            if len(items) > 500:
                lines.append('  ... (+%d more)' % (len(items) - 500))
        lines.append('')

    _sec('FILES ONLY IN A (missing from B)', only_a_files,
         lambda r: r)
    _sec('FILES ONLY IN B (extra in B)', only_b_files,
         lambda r: r)
    _sec('DIRS ONLY IN A (missing from B)', only_a_dirs,
         lambda r: r + '/')
    _sec('DIRS ONLY IN B (extra in B)', only_b_dirs,
         lambda r: r + '/')
    _sec('DIFFERENT SIZE (A bytes -> B bytes)', size_diff,
         lambda t: '%s   %d -> %d' % t)
    _sec('DIFFERENT HASH (same size, different content)', hash_diff,
         lambda t: '%s\n      A: %s\n      B: %s' % t)
    _sec('DIFFERENT MODE (chmod)', mode_diff,
         lambda t: '%s   %s -> %s' % (t[0], oct(t[1])[2:], oct(t[2])[2:]))
    _sec('DIFFERENT BSD FLAGS (chflags)', flag_diff,
         lambda t: '%s   0x%x -> 0x%x' % t)

    lines.append('-' * 72)
    lines.append('SUMMARY:')
    lines.append('  files only in A: %d' % len(only_a_files))
    lines.append('  files only in B: %d' % len(only_b_files))
    lines.append('  dirs only in A:  %d' % len(only_a_dirs))
    lines.append('  dirs only in B:  %d' % len(only_b_dirs))
    lines.append('  size mismatches: %d' % len(size_diff))
    lines.append('  hash mismatches: %d' % len(hash_diff))
    lines.append('  mode mismatches: %d' % len(mode_diff))
    lines.append('  flag mismatches: %d' % len(flag_diff))
    lines.append('')
    if not (only_a_files or only_b_files or only_a_dirs or only_b_dirs
            or size_diff or hash_diff or mode_diff or flag_diff):
        lines.append('NO DIFFERENCES FOUND in the file inventories. ')
        lines.append('Any remaining difference between the images must ')
        lines.append('be in the filesystem layout itself (block/frag ')
        lines.append('sizes, inode allocation, cylinder-group geometry, ')
        lines.append('etc). Compare image sizes above; if they differ ')
        lines.append('but contents match, this is the cause.')

    try:
        with open(path, 'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines))
    except Exception:
        # Last-ditch: write next to the system temp dir.
        alt = os.path.join(tempfile.gettempdir(),
            os.path.basename(path))
        with open(alt, 'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines))


# ─────────────────────────────────────────────────────────────────────
# Progress dialog (v2.0.6h) — shown during Apply backport's rebuild
# flow so the user can see what's happening and how long is left.
#
# Six stages with weighted contributions to the overall percentage:
#   1) Backup zip       —   5%
#   2) Extract image    —  30%
#   3) Overlay backport —   5%
#   4) Newfs init       —  15% (newfs's "Writing cylinder groups")
#   5) Newfs files      —  40% (newfs's "Adding files to image")
#   6) Swap + refresh   —   5%
#
# The dialog has:
#   • a stage label (top, bold)
#   • a detail line (sub-progress like "847 / 5612 files")
#   • a progressbar (0..100)
#   • elapsed time + ETA
#
# All state is pushed from the worker thread via `app.after(0, ...)`,
# so the dialog stays responsive while the rebuild runs.
# ─────────────────────────────────────────────────────────────────────
class _RebuildProgress:
    """Modal-ish progress dialog. Not strictly modal (we don't grab_set)
    so the user can still scroll the main window if they want — but
    it's always-on-top and the rebuild buttons are disabled by way of
    the work thread holding the only relevant entry-point."""

    STAGE_WEIGHTS = {
        'backup':  ( 0,  5),
        'extract': ( 5, 35),
        'overlay': (35, 40),
        'init':    (40, 55),
        'files':   (55, 95),
        'swap':    (95, 100),
    }

    def __init__(self, app, title, weights=None, initial_stage=None):
        self.app = app
        self.start_t = time.time()
        # v3.0.0: allow callers to override the stage weight map so
        # this dialog can be reused by the Convert tab and any other
        # multi-stage flow. If `weights` is provided it replaces the
        # class default; otherwise the rebuild flow's defaults apply.
        if weights is not None:
            self.STAGE_WEIGHTS = dict(weights)
        # Initial stage defaults to the first key in STAGE_WEIGHTS.
        if initial_stage is None:
            try:
                initial_stage = next(iter(self.STAGE_WEIGHTS))
            except StopIteration:
                initial_stage = 'init'
        self.stage = initial_stage
        self.stage_pct = 0.0
        # Rolling throughput for ETA — last N samples of (t, overall_pct)
        self._samples = []
        self._closed = False

        self.win = tk.Toplevel(app)
        self.win.title(title)
        self.win.configure(bg=COLORS['bg_1'])
        self.win.transient(app)
        self.win.resizable(False, False)
        self.win.protocol('WM_DELETE_WINDOW', lambda: None)   # no close

        w, h = 480, 180
        try:
            x = app.winfo_x() + (app.winfo_width()  - w) // 2
            y = app.winfo_y() + (app.winfo_height() - h) // 2
            self.win.geometry('%dx%d+%d+%d' % (w, h, x, y))
        except Exception:
            self.win.geometry('%dx%d' % (w, h))
        self.win.attributes('-topmost', True)

        body = tk.Frame(self.win, bg=COLORS['bg_1'])
        body.pack(fill='both', expand=True, padx=18, pady=16)

        self._stage_var  = tk.StringVar(value=_('Preparing...'))
        self._detail_var = tk.StringVar(value='')
        self._timing_var = tk.StringVar(value='')

        tk.Label(body, textvariable=self._stage_var,
                 font=('Segoe UI', 11, 'bold'),
                 bg=COLORS['bg_1'], fg=COLORS['fg_1'],
                 anchor='w').pack(fill='x', anchor='w')
        tk.Label(body, textvariable=self._detail_var,
                 font=('Segoe UI', 9),
                 bg=COLORS['bg_1'], fg=COLORS['fg_4'],
                 anchor='w').pack(fill='x', anchor='w', pady=(2, 12))

        # Progressbar. ttk.Progressbar inherits theme — we use a style
        # so the bar takes the accent colour.
        style = ttk.Style(self.win)
        try:
            style.configure('Rebuild.Horizontal.TProgressbar',
                background=COLORS['accent'],
                troughcolor=COLORS['bg_3'],
                bordercolor=COLORS['border_3'],
                lightcolor=COLORS['accent'],
                darkcolor=COLORS['accent'])
        except Exception:
            pass

        self._bar = ttk.Progressbar(body, mode='determinate',
            maximum=100, value=0,
            style='Rebuild.Horizontal.TProgressbar',
            length=440)
        self._bar.pack(fill='x', pady=(0, 8))

        tk.Label(body, textvariable=self._timing_var,
                 font=(FONTS['mono_sm'][0], 9),
                 bg=COLORS['bg_1'], fg=COLORS['fg_5'],
                 anchor='w').pack(fill='x', anchor='w')

        # Tick the timing line every 500ms so elapsed/ETA stay live
        # even when no progress update comes in (e.g. during the long
        # initial part of extract before the first file finishes).
        self._tick_after_id = None
        self._schedule_tick()

    # ── State updates ────────────────────────────────────────────────
    def set_stage(self, stage, label):
        """Called when entering a new stage. `label` is the user-facing
        bold line. Resets the stage-progress to 0."""
        if self._closed:
            return
        self.stage = stage
        self.stage_pct = 0.0
        try:
            self._stage_var.set(label)
            self._detail_var.set('')
            self._recompute_overall()
        except Exception:
            pass

    def set_stage_progress(self, pct, detail=None):
        """Update progress within the current stage. `pct` is 0..100."""
        if self._closed:
            return
        try:
            self.stage_pct = max(0.0, min(100.0, float(pct)))
            if detail is not None:
                self._detail_var.set(detail)
            self._recompute_overall()
        except Exception:
            pass

    def set_detail(self, detail):
        if self._closed:
            return
        try:
            self._detail_var.set(detail)
        except Exception:
            pass

    def close(self):
        if self._closed:
            return
        self._closed = True
        if self._tick_after_id:
            try:
                self.app.after_cancel(self._tick_after_id)
            except Exception:
                pass
            self._tick_after_id = None
        try:
            self.win.destroy()
        except Exception:
            pass

    # ── Internals ────────────────────────────────────────────────────
    def _recompute_overall(self):
        lo, hi = self.STAGE_WEIGHTS.get(self.stage, (0, 100))
        overall = lo + (hi - lo) * (self.stage_pct / 100.0)
        try:
            self._bar['value'] = overall
        except Exception:
            pass
        # Track sample for ETA
        now = time.time()
        self._samples.append((now, overall))
        # Keep last 30 samples (~ last 15 seconds at 2/sec)
        if len(self._samples) > 30:
            del self._samples[:-30]
        self._refresh_timing(now, overall)

    def _refresh_timing(self, now=None, overall=None):
        if now is None:
            now = time.time()
        if overall is None:
            try:
                overall = float(self._bar['value'])
            except Exception:
                overall = 0.0
        elapsed = now - self.start_t
        eta_str = ''
        # ETA from rolling average rate over the sample window.
        # Skip if too few samples or rate is tiny.
        if len(self._samples) >= 2 and overall > 1.0 and overall < 99.0:
            t0, p0 = self._samples[0]
            dt = now - t0
            dp = overall - p0
            if dt > 0.5 and dp > 0.1:
                rate = dp / dt
                remaining = max(0.0, 100.0 - overall)
                eta_s = remaining / rate
                if eta_s < 10 * 3600:
                    eta_str = _('  •  ETA %s') % _fmt_hms(eta_s)
        try:
            self._timing_var.set(
                _('Elapsed %s') % _fmt_hms(elapsed) + eta_str
                + ('  •  %d%%' % int(overall)))
        except Exception:
            pass

    def _schedule_tick(self):
        if self._closed:
            return
        self._refresh_timing()
        self._tick_after_id = self.app.after(500, self._schedule_tick)


def _fmt_hms(seconds):
    """Format seconds as e.g. '3m 12s' or '1h 04m 12s'."""
    s = int(max(0, seconds))
    h = s // 3600
    m = (s % 3600) // 60
    s = s % 60
    if h:
        return '%dh %02dm %02ds' % (h, m, s)
    if m:
        return '%dm %02ds' % (m, s)
    return '%ds' % s
