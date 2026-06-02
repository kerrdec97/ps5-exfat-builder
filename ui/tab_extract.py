"""ui/tab_extract.py — Extract image tab (v2: spacious Build-tab style).

Mount an .exfat image and copy its contents to a folder.

Layout (single column, no embedded log — uses global OUTPUT LOG):

    ┌─ Page head ───────────────────────────────────────────────────────┐
    │ [📤]  Extract image                                               │
    │       Mount + copy contents from an .exfat image                  │
    ├───────────────────────────────────────────────────────────────────┤
    │ [i] Mounts via OSFMount, mirrors files preserving structure...    │
    ├───────────────────────────────────────────────────────────────────┤
    │ ┌─ Drop zone ─────────────────────────────────────────────────┐ │
    │ │   📤  Drop .exfat image here                                │ │
    │ │       or click to browse                                    │ │
    │ └─────────────────────────────────────────────────────────────┘ │
    │ ┌─ Detected strip (when file selected) ───────────────────────┐ │
    │ │ 🎮 Returnal · CUSA-XXXXX · v1.04 · 56.8 GB                   │ │
    │ └─────────────────────────────────────────────────────────────┘ │
    │ ┌─ Form fields ───────────────────────────────────────────────┐ │
    │ │ Source image                                                │ │
    │ │ [_______________________________________________] [Browse]  │ │
    │ │                                                             │ │
    │ │ Output directory                                            │ │
    │ │ [_______________________________________________] [Browse]  │ │
    │ │                                                             │ │
    │ │ Output folder name  • auto from filename                    │ │
    │ │ [_______________________________________________]           │ │
    │ │                                                             │ │
    │ │ ┌─ Progress card ────────────────────────────────────────┐ │ │
    │ │ │ status text         [████──────] ETA xx:xx              │ │ │
    │ │ └────────────────────────────────────────────────────────┘ │ │
    │ │                                                             │ │
    │ │ [📤 Extract Image]                                          │ │
    │ └─────────────────────────────────────────────────────────────┘ │
    └───────────────────────────────────────────────────────────────────┘

Output goes to the global OUTPUT LOG at the bottom (no embedded log
duplicating it).

Backwards compat: every `_extract_*` attribute the existing 5+ callbacks
read is preserved with the same name. Worker logic untouched.
"""

import os
import re
import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _

from ui.shared.cards import DetectedGameStrip
from ui.shared.forms import DropZone
from ui.shared.log_view import ConsoleView   # kept for compat; not packed
from ui.shared.page_head import (
    make_themed_button, info_banner, page_head, field_block)


def build_extract_tab(parent, app):
    parent.configure(bg=COLORS['bg_1'])

    # State
    app._extract_file_var   = tk.StringVar()
    app._extract_outdir_var = tk.StringVar()
    app._extract_name_var   = tk.StringVar(value='')
    app._extract_status_var = tk.StringVar(value='')
    app._extract_eta_var    = tk.StringVar(value='')
    app._extract_pct        = 0

    # ── Scrollable wrap ──
    canvas = tk.Canvas(parent, bg=COLORS['bg_1'], bd=0,
                       highlightthickness=0)
    canvas.pack(side='left', fill='both', expand=True)
    sb = tk.Scrollbar(parent, orient='vertical', command=canvas.yview,
                       bg=COLORS['bg_2'], troughcolor=COLORS['bg_1'])
    sb.pack(side='right', fill='y')
    canvas.configure(yscrollcommand=sb.set)

    inner = tk.Frame(canvas, bg=COLORS['bg_1'])
    inner_id = canvas.create_window((0, 0), window=inner, anchor='nw')
    inner.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig(inner_id, width=e.width))
    canvas.bind('<MouseWheel>', lambda e:
        canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

    # ── Page head ──
    head = page_head(inner, '\U0001f4e4',
                     _('Extract image'),
                     _('Mount and copy contents from an .exfat image.'))
    head.pack(fill='x', padx=24, pady=(14, 12))

    # Force Dismount button on the right of the head
    def _force_dismount():
        try:
            app._force_dismount_all()
        except Exception:
            pass
    fd_btn = make_themed_button(head, '\u26a0  ' + _('Force Dismount'),
                                  command=_force_dismount, kind='ghost')
    fd_btn.pack(side='right', padx=(8, 0))

    # ── Info banner ──
    banner = info_banner(inner, _(
        'Mounts via OSFMount, then mirrors files preserving folder '
        'structure. Requires roughly the image size in free space on '
        'the output drive. Output goes to the global OUTPUT LOG (click '
        'at the bottom to expand).'))
    banner.pack(fill='x', padx=24, pady=(0, 14))

    # ── Card ──
    card = tk.Frame(inner, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1)
    card.pack(fill='x', padx=24, pady=(0, 14))

    pad = tk.Frame(card, bg=COLORS['bg_2'])
    pad.pack(fill='x', padx=24, pady=18)

    # Drop zone
    dz = DropZone(pad,
                  on_drop=lambda p: _on_drop(app, p),
                  on_click=app._browse_extract_file,
                  hint=_('or click to browse'),
                  glyph='\U0001f4e4')
    dz.pack(fill='x', pady=(0, 12))
    try:
        dz._main_lbl.configure(text=_('Drop .exfat image here'))
    except Exception:
        pass

    # Detected strip (populated when file selected)
    app._extract_info_strip = DetectedGameStrip(pad, kind='accent')

    # Form fields
    field_block(pad, _('Source image'),
                 var=app._extract_file_var,
                 on_browse=app._browse_extract_file,
                 browse_text=_('Browse'),
                 hint=_('the .exfat image to extract'))

    field_block(pad, _('Output directory'),
                 var=app._extract_outdir_var,
                 on_browse=app._browse_extract_outdir,
                 browse_text=_('Browse'),
                 hint=_('where extracted files will land'))

    # Output folder name with auto-detected hint
    name_block = tk.Frame(pad, bg=COLORS['bg_2'])
    name_block.pack(fill='x', pady=(14, 0))
    name_lbl_row = tk.Frame(name_block, bg=COLORS['bg_2'])
    name_lbl_row.pack(fill='x')
    tk.Label(name_lbl_row, text=_('Output folder name'),
             font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'], anchor='w'
             ).pack(side='left')
    app._extract_auto_lbl = tk.Label(name_lbl_row,
        text='  \u2022  ' + _('auto from filename'),
        font=FONTS['meta'],
        bg=COLORS['bg_2'], fg=COLORS['success_hi'])
    app._extract_auto_lbl.pack(side='left')

    name_wrap = tk.Frame(name_block, bg=COLORS['field_bg'],
                          highlightbackground=COLORS['border_3'],
                          highlightthickness=1)
    name_wrap.pack(fill='x', pady=(6, 0))
    tk.Entry(name_wrap, textvariable=app._extract_name_var,
             font=FONTS['mono_sm'],
             bg=COLORS['field_bg'], fg=COLORS['field_fg'],
             insertbackground=COLORS['field_fg'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=8
             ).pack(fill='x')

    # ── Progress card ──
    prog_card = tk.Frame(pad, bg=COLORS['bg_3'],
                          highlightbackground=COLORS['border_2'],
                          highlightthickness=1)
    prog_card.pack(fill='x', pady=(18, 14))
    pi = tk.Frame(prog_card, bg=COLORS['bg_3'])
    pi.pack(fill='x', padx=14, pady=12)

    status_row = tk.Frame(pi, bg=COLORS['bg_3'])
    status_row.pack(fill='x')
    tk.Label(status_row, textvariable=app._extract_status_var,
             font=FONTS['body_b'],
             bg=COLORS['bg_3'], fg=COLORS['fg_0'], anchor='w'
             ).pack(side='left', fill='x', expand=True)
    tk.Label(status_row, textvariable=app._extract_eta_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_3'], fg=COLORS['fg_4'], anchor='e'
             ).pack(side='right')

    # Slim progress bar
    bar_bg = tk.Frame(pi, bg=COLORS['bg_4'], height=8)
    bar_bg.pack(fill='x', pady=(8, 0))
    bar_bg.pack_propagate(False)
    app._extract_canvas = tk.Canvas(bar_bg, height=8, bg=COLORS['bg_4'],
                                     highlightthickness=0)
    app._extract_canvas.pack(fill='both', expand=True)
    app._extract_bar = app._extract_canvas.create_rectangle(
        0, 0, 0, 8, fill=COLORS['accent'], outline='')
    app._extract_canvas.bind('<Configure>',
        lambda e: app._update_extract_bar(app._extract_pct))

    # ── Action button ──
    app._extract_btn = make_themed_button(
        pad,
        text=_('Extract Image'),
        command=app._run_extract,
        kind='success',
        icon='\U0001f4e4',
        font_size=11, padx=20, pady=10)
    app._extract_btn.pack(anchor='w', pady=(0, 0))

    # ── Hidden ConsoleView for back-compat ──
    # Legacy code paths (mostly in exfat_builder.py) still expect to
    # find `app._extract_log` as a Text widget. We create one but never
    # pack it — the actual output goes to the global OUTPUT LOG via the
    # `_extract_log_to_global` hook below.
    hidden_holder = tk.Frame(inner, bg=COLORS['bg_1'])
    # Don't pack; it stays as a no-op holder
    _cv = ConsoleView(hidden_holder, height=10)
    app._extract_log = _cv.text
    app._extract_log_console = _cv

    # Wire the hidden console: anything written to it also flows to
    # the global OUTPUT LOG so users see what's happening.
    _install_extract_log_mirror(app)

    # Trace the source-image var so the detected strip auto-populates
    app._extract_file_var.trace_add('write',
        lambda *a: _refresh_detected(app))


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _on_drop(app, path):
    """Drop handler — accept a single .exfat file."""
    if path and path.lower().endswith('.exfat') and os.path.isfile(path):
        app._extract_file_var.set(path)
        base = os.path.splitext(os.path.basename(path))[0]
        app._extract_name_var.set(base)


def _refresh_detected(app):
    """Repaint the detected strip from the current source filename."""
    strip = getattr(app, '_extract_info_strip', None)
    if strip is None:
        return
    path = app._extract_file_var.get().strip()
    if not path or not os.path.isfile(path):
        try:
            strip.pack_forget()
        except Exception:
            pass
        return

    base = os.path.basename(path)
    name = base.rsplit('.', 1)[0]
    gid = None
    m = re.search(r'((?:CUSA|PPSA|PPLH)[-_ ]?\d{5})', name, re.IGNORECASE)
    if m:
        gid = m.group(1).upper().replace('_', '-').replace(' ', '-')
        if '-' not in gid:
            gid = gid[:4] + '-' + gid[4:]
    ver = None
    m = re.search(r'v(\d+\.\d+)', name, re.IGNORECASE)
    if m:
        ver = m.group(1)
    title = name
    if gid:
        title = re.sub(r'(?:CUSA|PPSA|PPLH)[-_ ]?\d{5}', '', title,
                       flags=re.IGNORECASE)
    if ver:
        title = re.sub(r'v\d+\.\d+', '', title, flags=re.IGNORECASE)
    title = re.sub(r'^[\s\-_.]+|[\s\-_.]+$', '', title).strip() or name

    try:
        size = os.path.getsize(path)
        if size >= 1024**3:
            size_str = '%.1f GB' % (size / 1024**3)
        else:
            size_str = '%d MB' % (size // 1024**2)
    except Exception:
        size_str = ''

    strip.title_var.set(title)
    strip.id_var.set(gid or '')
    strip.ver_var.set(('v' + ver) if ver else '')
    strip.size_var.set(size_str)

    try:
        if not strip.winfo_ismapped():
            # Pack between drop zone (children[0]) and first field
            strip.pack(fill='x', pady=(0, 12),
                       in_=strip.master,
                       before=strip.master.winfo_children()[1])
    except Exception:
        try:
            strip.pack(fill='x', pady=(0, 12))
        except Exception:
            pass


def _install_extract_log_mirror(app):
    """Hook the hidden _extract_log Text widget so any insert() call
    is mirrored to the global OUTPUT LOG. Idempotent per process."""
    if getattr(app, '_extract_log_mirror_installed', False):
        return
    app._extract_log_mirror_installed = True

    txt = getattr(app, '_extract_log', None)
    if txt is None:
        return

    original_insert = txt.insert

    def _wrapped_insert(*args, **kwargs):
        try:
            original_insert(*args, **kwargs)
        except Exception:
            pass
        # Mirror to global log. args is (index, chars, [tag]) typically.
        try:
            chars = args[1] if len(args) >= 2 else kwargs.get('chars', '')
            if chars and hasattr(app, '_log'):
                line = chars if chars.endswith('\n') else chars + '\n'
                if not line.startswith('[EXTRACT]') and \
                        not line.startswith('['):
                    line = '[EXTRACT] ' + line
                app._log(line)
                try:
                    app._toggle_log(force_open=True)
                except Exception:
                    pass
        except Exception:
            pass

    txt.insert = _wrapped_insert
