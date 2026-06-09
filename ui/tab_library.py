"""
ui/tab_library.py — Game Library tab.

Step 5 (v2.0.7): refactored against preview/library-tab-redesign.html.

Layout (top to bottom):

    [Game Library]  [47 games · 3 folders]    [+ Add folder] [↻ Scan]
                                               [⚡ Backport All] [🔨 Build All]
    ─────────────────────────────────────────────────────────────────
    [🔍 Filter…]  [⚏ Grid] [☰ List]                            [Status]
    ─────────────────────────────────────────────────────────────────
    [📁 D:/PS5/Dumps  28 games  ✕]  [📁 E:/Backports  ...]  ←── chip strip
    ─────────────────────────────────────────────────────────────────
    [● Scanning  ▰▰▰▰▰▰▰▱▱▱  29/47  elapsed 00:14]   ← scan bar (when running)
    ─────────────────────────────────────────────────────────────────
    [GameCard][GameCard][GameCard][GameCard]   ← grid (auto-fill 170px)
    [GameCard][GameCard][GameCard][GameCard]
    ...

Backwards compat: every `app._lib_*` widget attribute the existing
callbacks reference is preserved with the same name. The card-rendering
helper (`_lib_make_card`) is rewritten in-place in exfat_builder.py to use
the new GameCard widget — that's a UI-rendering rewrite, not a behavior
change. Worker logic (`_lib_scan`, `_lib_apply_filter`, etc.) is untouched.

Deferred (out of scope for this iteration; would require modifying
`_lib_apply_filter` callback):
  - Filter pills (All/Built/Unbuilt/Backportable)
  - Sort dropdown (Title A→Z / Recently added / Size)
"""

import tkinter as tk

from tkinter_theme import COLORS, FONTS

# Star-import provides legacy theme constants and i18n function.
from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _


def build_library_tab(parent, app):
    """Build the redesigned Library tab body into `parent`."""
    parent.configure(bg=COLORS['bg_1'])
    app._lib_view_mode = tk.StringVar(value='grid')
    app._lib_search_var = tk.StringVar()

    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    _build_page_head(body, app)
    _build_toolbar(body, app)
    _build_folder_strip(body, app)
    _build_scan_bar(body, app)
    _build_footer_stats(body, app)   # packed bottom before the grid expands
    _build_grid_area(body, app)

    # Step 41 (v2.5.7): paint the scan-folder chips immediately so
    # users can see and remove folders without first running a scan.
    # Without this, the chip strip stayed empty after app launch
    # until you added/removed a folder, making it look like there
    # was no way to manage scan folders. Deferred one frame so all
    # widgets exist before _lib_render_folders touches them.
    try:
        parent.after(0, app._lib_render_folders)
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# Page head — title + count pill + action buttons
# ─────────────────────────────────────────────────────────────────────────
def _build_page_head(body, app):
    head = tk.Frame(body, bg=COLORS['bg_1'])
    head.pack(fill='x', padx=24, pady=(14, 12))

    tk.Label(head, text=_('Game Library'),
             font=(FONTS['h2'][0], 15, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0']
             ).pack(side='left')

    # Stat badges — fed by _lib_scan_done / _lib_render_folders (guarded
    # hasattr on the monolith side, so older shells still work).
    app._lib_count_var = tk.StringVar(value='0 games')   # legacy compat
    app._lib_stat_games_var = tk.StringVar(value='0')
    app._lib_stat_folders_var = tk.StringVar(value='0')

    def _badge(var, suffix):
        f = tk.Frame(head, bg=COLORS['bg_2'],
                     highlightbackground=COLORS['border_2'],
                     highlightthickness=1)
        f.pack(side='left', padx=(10, 0))
        tk.Label(f, textvariable=var,
                 font=(FONTS['mono_sm'][0], 9, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['fg_0']
                 ).pack(side='left', padx=(8, 2), pady=2)
        tk.Label(f, text=suffix, font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4']
                 ).pack(side='left', padx=(0, 8), pady=2)
    _badge(app._lib_stat_games_var, _('games'))
    _badge(app._lib_stat_folders_var, _('folders'))

    # Right-aligned actions
    _btn(head, '\U0001f528  Build All',
         command=app._lib_build_all, kind='success'
         ).pack(side='right')
    _btn(head, '\u26a1  Backport All',
         command=app._lib_backport_all, kind='purple'
         ).pack(side='right', padx=(0, 6))
    _btn(head, '\u21bb  Scan Folders',
         command=app._lib_scan, kind='primary'
         ).pack(side='right', padx=(0, 6))
    _btn(head, '+  Add Folder',
         command=app._lib_add_folder, kind='ghost'
         ).pack(side='right', padx=(0, 6))


# ─────────────────────────────────────────────────────────────────────────
# Toolbar — search + view-mode seg + status text
# ─────────────────────────────────────────────────────────────────────────
def _build_toolbar(body, app):
    bar = tk.Frame(body, bg=COLORS['bg_0'],
                   highlightbackground=COLORS['border_2'],
                   highlightthickness=1)
    bar.pack(fill='x')
    inner = tk.Frame(bar, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=24, pady=10)

    # ── Search input (prominent, full-width) ──
    search_wrap = tk.Frame(inner, bg=COLORS['field_bg'],
                           highlightbackground=COLORS['border_3'],
                           highlightthickness=1)
    search_wrap.pack(side='left', fill='x', expand=True)
    tk.Label(search_wrap, text='\U0001f50d',
             font=(FONTS['body'][0], 11),
             bg=COLORS['field_bg'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(10, 6))
    se = tk.Entry(search_wrap, textvariable=app._lib_search_var,
                  font=(FONTS['body'][0], 10),
                  bg=COLORS['field_bg'], fg=COLORS['field_fg'],
                  insertbackground=COLORS['accent'],
                  selectbackground=COLORS['accent'],
                  selectforeground=COLORS['fg_0'],
                  relief='flat', bd=0)
    se.pack(side='left', fill='x', expand=True, ipady=7, padx=(0, 10))
    # placeholder-style hint via empty-state fg swap is out of scope;
    # the magnifier glyph carries the affordance.
    app._lib_search_var.trace('w', lambda *a: app._lib_apply_filter())

    # ── View mode seg (Grid / List) ──
    seg = tk.Frame(inner, bg=COLORS['bg_0'],
                   highlightbackground=COLORS['border_3'],
                   highlightthickness=1)
    seg.pack(side='left', padx=(12, 0), ipady=2)

    def _make_seg_btn(text, mode):
        btn = tk.Label(seg, text=text,
                       font=FONTS['body'],
                       bg=COLORS['bg_0'], fg=COLORS['fg_4'],
                       padx=12, pady=4, cursor='hand2')
        btn.pack(side='left', padx=2)

        def _click(_e=None):
            app._lib_view_mode.set(mode)
            for w in seg.winfo_children():
                if isinstance(w, tk.Label):
                    w.configure(bg=COLORS['bg_0'], fg=COLORS['fg_4'])
            btn.configure(bg=COLORS['bg_3'], fg=COLORS['fg_0'])
            app._lib_apply_filter()

        btn.bind('<Button-1>', _click)
        return btn, _click

    app._lib_grid_btn, _grid_click = _make_seg_btn('\u22ee\u22ee  Grid', 'grid')
    app._lib_list_btn, _list_click = _make_seg_btn('\u2630  List', 'list')
    # Default: Grid is active
    app._lib_grid_btn.configure(bg=COLORS['bg_3'], fg=COLORS['fg_0'])

    # ── Spacer + status text (right-aligned) ──
    app._lib_status_var = tk.StringVar(
        value=_('No games scanned yet — add a folder and click Scan'))
    tk.Label(inner, textvariable=app._lib_status_var,
             font=FONTS['meta'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='right')


# ─────────────────────────────────────────────────────────────────────────
# Folder chip strip
# ─────────────────────────────────────────────────────────────────────────
def _build_folder_strip(body, app):
    """Horizontal scrollable strip of FolderChips — one per scan folder.

    Includes a small label so users know what the chips are and that
    each one has an ✕ button to stop scanning that folder.
    """
    outer = tk.Frame(body, bg=COLORS['bg_0'])
    outer.pack(fill='x')

    # Label row: "Scanning folders:" + a hint
    header = tk.Frame(outer, bg=COLORS['bg_0'])
    header.pack(fill='x', padx=24, pady=(2, 4))

    tk.Label(header, text=_('Scanning folders:'),
             font=(FONTS['body'][0], 9, 'bold'),
             bg=COLORS['bg_0'], fg=COLORS['fg_2']
             ).pack(side='left')
    tk.Label(header,
             text=_('(click \u2715 to stop scanning a folder)'),
             font=(FONTS['body'][0], 8),
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(8, 0))

    # `_lib_folder_frame` is the named slot the existing _lib_render_folders
    # callback writes chips into. Same name as before; new visual style.
    app._lib_folder_frame = tk.Frame(outer, bg=COLORS['bg_0'])
    app._lib_folder_frame.pack(fill='x', padx=24, pady=(0, 10))


# ─────────────────────────────────────────────────────────────────────────
# Scan progress bar (hidden until scan starts)
# ─────────────────────────────────────────────────────────────────────────
def _build_scan_bar(body, app):
    """Slim scan progress bar — pack/forget controlled by callbacks."""
    app._lib_progress_frame = tk.Frame(body, bg=COLORS['bg_0'])
    app._lib_progress_frame.pack(fill='x')

    inner = tk.Frame(app._lib_progress_frame, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=24, pady=(0, 8))

    # Tiny dot + scanning label
    tk.Label(inner, text='\u25cf',
             font=(FONTS['body'][0], 8),
             bg=COLORS['bg_0'], fg=COLORS['accent']
             ).pack(side='left', padx=(0, 6))
    tk.Label(inner, text=_('Scanning…'),
             font=FONTS['meta'],
             bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='left')

    # The progress bar itself — same widget structure as before so the
    # existing _lib_set_scan_progress callback works unchanged.
    bar_bg = tk.Frame(inner, bg=COLORS['bg_2'], height=4)
    bar_bg.pack(side='left', fill='x', expand=True, padx=(12, 12))
    bar_bg.pack_propagate(False)
    app._lib_prog_bar_bg = bar_bg
    app._lib_prog_canvas = tk.Canvas(bar_bg, height=4, bg=COLORS['bg_2'],
                                     highlightthickness=0, bd=0)
    app._lib_prog_canvas.pack(fill='both', expand=True)
    app._lib_prog_fill = app._lib_prog_canvas.create_rectangle(
        0, 0, 0, 4, fill=COLORS['accent'], outline='')

    # Hide until scan starts (existing pattern from old tab)
    app._lib_progress_frame.pack_forget()


# ─────────────────────────────────────────────────────────────────────────
# Footer stats strip — GAMES / FOLDERS tiles + last scan + Rescan
# ─────────────────────────────────────────────────────────────────────────
def _build_footer_stats(body, app):
    bar = tk.Frame(body, bg=COLORS['bg_0'],
                   highlightbackground=COLORS['border_2'],
                   highlightthickness=1)
    bar.pack(fill='x', side='bottom')
    inner = tk.Frame(bar, bg=COLORS['bg_0'])
    inner.pack(fill='x', padx=24, pady=10)

    def _tile(glyph, glyph_fg, var, caption):
        t = tk.Frame(inner, bg=COLORS['bg_0'])
        t.pack(side='left', padx=(0, 36))
        tk.Label(t, text=glyph, font=(FONTS['body'][0], 16),
                 bg=COLORS['bg_0'], fg=glyph_fg
                 ).pack(side='left', padx=(0, 10))
        col = tk.Frame(t, bg=COLORS['bg_0'])
        col.pack(side='left')
        tk.Label(col, text=caption.upper(),
                 font=(FONTS['mono_sm'][0], 8, 'bold'),
                 bg=COLORS['bg_0'], fg=COLORS['fg_5'], anchor='w'
                 ).pack(anchor='w')
        tk.Label(col, textvariable=var,
                 font=(FONTS['h3'][0], 14, 'bold'),
                 bg=COLORS['bg_0'], fg=COLORS['fg_0'], anchor='w'
                 ).pack(anchor='w')

    _tile('\U0001f3ae', COLORS['accent'], app._lib_stat_games_var,
          _('Games'))
    _tile('\U0001f4c1', COLORS['warn'], app._lib_stat_folders_var,
          _('Folders'))

    _btn(inner, '\u21bb  ' + _('Rescan'),
         command=app._lib_scan, kind='ghost').pack(side='right')
    app._lib_lastscan_var = tk.StringVar(value=_('Last scan: \u2014'))
    tk.Label(inner, textvariable=app._lib_lastscan_var,
             font=FONTS['meta'], bg=COLORS['bg_0'], fg=COLORS['fg_4']
             ).pack(side='right', padx=(0, 14))


# ─────────────────────────────────────────────────────────────────────────
# Card grid (scrollable canvas, populated by _lib_render_grid)
# ─────────────────────────────────────────────────────────────────────────
def _build_grid_area(body, app):
    grid_outer = tk.Frame(body, bg=COLORS['bg_1'])
    grid_outer.pack(fill='both', expand=True, padx=24, pady=(0, 12))

    canvas = tk.Canvas(grid_outer, bg=COLORS['bg_1'], highlightthickness=0)
    sb = tk.Scrollbar(grid_outer, orient='vertical',
                      command=canvas.yview,
                      bg=COLORS['bg_2'], troughcolor=COLORS['bg_1'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    app._lib_canvas = canvas
    app._lib_grid_frame = tk.Frame(canvas, bg=COLORS['bg_1'])
    canvas.create_window((0, 0), window=app._lib_grid_frame,
                         anchor='nw', tags='lgf')
    app._lib_grid_frame.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e: (
        canvas.itemconfig('lgf', width=e.width),
        app._lib_apply_filter()))
    canvas.bind('<MouseWheel>',
        lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _btn(parent, text, command, kind='ghost'):
    """Themed tk.Button — same kind→colors mapping as in tab_klog."""
    schemes = {
        'primary': (COLORS['accent'],  COLORS['fg_0'],
                    COLORS['accent_hi'], COLORS['fg_0']),
        'success': (COLORS['success'], COLORS['fg_0'],
                    COLORS['success_hi'], COLORS['fg_0']),
        'purple':  (COLORS['purple'],  COLORS['fg_0'],
                    '#a567c1',          COLORS['fg_0']),
        'ghost':   (COLORS['bg_2'],    COLORS['fg_2'],
                    COLORS['bg_3'],     COLORS['fg_0']),
    }
    bg, fg, abg, afg = schemes.get(kind, schemes['ghost'])
    btn = tk.Button(parent, text=text,
                    font=(FONTS['button'][0], 9, 'bold'),
                    bg=bg, fg=fg,
                    activebackground=abg, activeforeground=afg,
                    relief='flat', bd=0,
                    padx=12, pady=6,
                    cursor='hand2',
                    command=command)
    if kind == 'ghost':
        btn.configure(highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
    return btn
