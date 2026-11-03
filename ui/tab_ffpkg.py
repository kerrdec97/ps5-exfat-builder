"""
ui/tab_ffpkg.py — ffpkg image builder tab (UFS2Tool, .NET 8 dependency).

Design-system layout (sibling of the unified Build screen):

    Page header  "Build ffpkg image"                      [UFS2 · S=512 fixed]
    ┌ Source ──────────────────────────────┐ ┌ Build progress ─────────────┐
    │ (i) UFS2 / sector 512 note           │ │ ✓ Write ▶ Copy ○ Finalise   │
    │ .NET 8 runtime ············ [Check]  │ │ ▬▬▬▬▬▬▬▬▬▬──────────── 55%  │
    │ drop strip                           │ │ step · ETA                  │
    │ detected game                        │ └─────────────────────────────┘
    │ Output directory   [_______][Browse] │ ┌ Queue ───────────── [Clear] ┐
    │ Output filename    (read-only)       │ │ rows: status · name · badge │
    ├──────────────────────────────────────┤ └─────────────────────────────┘
    │                  Add to queue [Build]│
    └──────────────────────────────────────┘
    ┌ UFS2 parameters (read-only) ─────────┐

Backwards compat: every `app._ffpkg_*` attribute the existing callbacks
read or write is preserved with the same name. Worker logic untouched.
"""

import os
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)

from tkinter_theme import COLORS, FONTS, px

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.shared.log_view import ConsoleView
from ui.shared.page_head import field_block
from ui.shared.scroll import attach_scroll
from ui.shared.widgets import (Button, Card, Badge, Divider, InfoBar,
                               PageHeader, Rounded, IconLabel)
from ui.tab_extract import (scroll_page, page_columns, DropStrip,
                            DetectedStrip, StageTracker, caption)

C = COLORS
BG1, BG2 = COLORS['bg_1'], COLORS['bg_2']


def build_ffpkg_tab(parent, app):
    """Build the ffpkg Build sub-tab body into `parent`."""
    parent.configure(bg=BG1)

    # ── State ──
    app._ffpkg_game_folder = tk.StringVar()
    app._ffpkg_output_dir  = tk.StringVar(
        value=app._settings.get('ffpkg_output_dir', ''))
    app._ffpkg_output_name = tk.StringVar(value='game.ffpkg')
    app._ffpkg_step_var    = tk.StringVar(value='')
    app._ffpkg_pct_var     = tk.StringVar(value='')
    app._ffpkg_eta_var     = tk.StringVar(value='')

    page = scroll_page(parent)
    head = PageHeader(page, _('Build ffpkg image'),
                      _('Pick a game folder and queue the build.'))
    head.pack(fill='x', padx=px(24), pady=(px(20), px(16)))
    # Mode indicator on the right
    Badge(head.actions, _('UFS2 · S=512 fixed'), 'outline', mono=True,
          surface=BG1).pack(side='right')

    left, right = page_columns(page)
    _build_left_column(left, app)
    _build_right_column(right, app)
    # Paint the (empty) queue once so the card shows its empty state.
    try:
        if not getattr(app, '_ffpkg_queue', None):
            app._ffpkg_render_queue()
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# LEFT COLUMN — build form
# ─────────────────────────────────────────────────────────────────────────
def _build_left_column(col, app):
    card = Card(col, title=_('Source'),
                subtitle=_('Creates UFS2 (.ffpkg) images using UFS2Tool with sector '
                           'size locked at 512 bytes for Windows compatibility.'),
                actions=True)
    card.pack(fill='x')
    pad = card.body

    # ── .NET runtime check row ──
    dotnet = Rounded(pad, bg=C['bg_1'], border=C['border_2'], radius=8,
                     padding=(px(12), px(8)), surface=BG2)
    dotnet.pack(fill='x', pady=(0, px(12)))
    IconLabel(dotnet, 'cpu', 16, C['fg_3'], C['bg_1']).pack(
        side='left', padx=(0, px(10)))
    app._ffpkg_dotnet_status_var = tk.StringVar(
        value=_('.NET 8 runtime — click to verify'))
    tk.Label(dotnet, textvariable=app._ffpkg_dotnet_status_var,
             font=FONTS['body'], bg=C['bg_1'], fg=C['fg_1'],
             anchor='w').pack(side='left', fill='x', expand=True)
    # Right-side button to manually re-check
    Button(dotnet, text=_('Check'), size='sm', command=app._ffpkg_check_dotnet,
           surface=C['bg_1']).pack(side='right')

    # ── Drop strip ──
    drop = DropStrip(pad, _('Drop a game folder here'),
                     on_drop=lambda p: _on_drop(app, p),
                     on_click=app._ffpkg_browse_game,
                     hint=_('Folder must contain eboot.bin'),
                     icon='folder_in')
    drop.pack(fill='x', pady=(0, px(12)))

    # ── Detected game strip ──
    # Same parent as the output-dir field: the legacy detector packs it
    # with before=_ffpkg_output_dir_frame.
    strip = DetectedStrip(pad)
    # NOT packed yet — the legacy `_ffpkg_detect_game` packs it via the
    # `_ffpkg_info_frame` alias below.

    # Backwards-compat aliases — every name the legacy code expects:
    app._ffpkg_info_frame     = strip
    app._ffpkg_cover_label    = strip.cover_label
    app._ffpkg_info_title_var = strip.title_var
    app._ffpkg_info_id_var    = strip.id_var
    app._ffpkg_info_ver_var   = strip.ver_var
    app._ffpkg_info_size_var  = strip.size_var
    app._ffpkg_cover_image    = None

    # ── Output directory field ──
    app._ffpkg_output_dir_frame = _field(pad,
        _('Output directory'),
        var=app._ffpkg_output_dir,
        on_browse=app._ffpkg_browse_output)

    # ── Output filename preview (read-only) ──
    fn_block = tk.Frame(pad, bg=BG2)
    fn_block.pack(fill='x')
    fn_label_row = tk.Frame(fn_block, bg=BG2)
    fn_label_row.pack(fill='x')
    tk.Label(fn_label_row, text=_('Output filename'), font=FONTS['body'],
             bg=BG2, fg=C['fg_2'], anchor='w').pack(side='left')
    _auto_slot = tk.Frame(fn_label_row, bg=BG2)
    _auto_slot.pack(side='left', padx=(px(8), 0))
    app._ffpkg_auto_label = tk.Label(_auto_slot,
                                     text=_('auto-detected'),
                                     font=FONTS['caption'], bg=BG2,
                                     fg=C['success_hi'], anchor='w')
    app._ffpkg_auto_label.pack(side='left')
    app._ffpkg_auto_label.pack_forget()

    fn_input = Rounded(fn_block, bg=C['bg_1'], border=C['border_2'],
                       radius=4, padding=(px(12), px(7)), surface=BG2)
    fn_input.pack(fill='x', pady=(px(4), 0))
    app._ffpkg_filename_preview = tk.Label(fn_input,
                                           textvariable=app._ffpkg_output_name,
                                           font=FONTS['code'],
                                           bg=C['bg_1'], fg=C['fg_1'],
                                           anchor='w')
    app._ffpkg_filename_preview.pack(fill='x')

    # ── Action buttons (card footer) ──
    actions = card.actions
    app._ffpkg_build_btn = _teal_btn(actions,
        '\U0001f528  ' + _('Build All'),
        command=app._ffpkg_run_queue)
    app._ffpkg_build_btn.pack(side='right')
    _accent_btn(actions, _('Add to queue'),
                command=app._ffpkg_add_to_queue
                ).pack(side='right', padx=(0, px(6)))

    # ── UFS2 parameters card (read-only, sourced from advanced settings) ──
    _build_params_card(col, app)


def _build_params_card(parent, app):
    """Read-only UFS2 parameters card. Values sourced live from
    `_adv_ffpkg_*` vars; the -S row is locked at 512."""
    card = Card(parent, title=_('UFS2 parameters'),
                subtitle=_('Customise these in the Advanced tab.'))
    card.pack(fill='x', pady=(px(16), 0))
    Button(card.head_actions, text=_('Advanced'), icon='advanced', size='sm',
           variant='subtle', command=lambda: app._switch_tab('advanced'),
           surface=BG2).pack()

    rows = {'n': 0}

    # Row helper — 3-column grid: [description] [flag] [value]
    def _param_row(key, desc, value_var, locked=False):
        if rows['n']:
            Divider(card.body, color=C['border_1']).pack(fill='x')
        rows['n'] += 1
        row = tk.Frame(card.body, bg=BG2)
        row.pack(fill='x', pady=px(6))
        row.grid_columnconfigure(0, weight=1)
        tk.Label(row, text=desc, font=FONTS['body'], bg=BG2, fg=C['fg_1'],
                 anchor='w').grid(row=0, column=0, sticky='w')
        tk.Label(row, text=key, font=FONTS['code'], bg=BG2, fg=C['fg_4'],
                 anchor='e', width=6).grid(row=0, column=1, sticky='e',
                                           padx=(0, px(16)))
        vcell = tk.Frame(row, bg=BG2)
        vcell.grid(row=0, column=2, sticky='e')
        if locked:
            IconLabel(vcell, 'lock', 12, C['fg_3'], BG2).pack(
                side='left', padx=(0, px(6)))
        if isinstance(value_var, tk.StringVar):
            val_lbl = tk.Label(vcell, textvariable=value_var,
                               font=FONTS['code'], bg=BG2, fg=C['fg_0'],
                               anchor='e', width=8)
        else:
            val_lbl = tk.Label(vcell, text=str(value_var), font=FONTS['code'],
                               bg=BG2, fg=C['fg_0'], anchor='e', width=8)
        val_lbl.pack(side='left')

    # ── Static rows ──
    _param_row('-O 2', _('UFS version'), '2')
    # Live rows from advanced settings
    _param_row('-b', _('Block size'),
               getattr(app, '_adv_ffpkg_block_var', tk.StringVar(value='32768')))
    _param_row('-f', _('Fragment size'),
               getattr(app, '_adv_ffpkg_frag_var', tk.StringVar(value='4096')))
    _param_row('-m', _('Min free %'),
               getattr(app, '_adv_ffpkg_minfree_var', tk.StringVar(value='8')))
    _param_row('-S', _('Sector size'), '512', locked=True)
    _param_row('-i', _('Inode density'),
               getattr(app, '_adv_ffpkg_inode_var', tk.StringVar(value='4096')))


def _on_drop(app, path):
    """Handler for the drop strip — sets game folder and triggers detection."""
    if not os.path.isdir(path):
        return
    try:
        app._ffpkg_game_folder.set(path)
        app._ffpkg_detect_game_async(path)
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# RIGHT COLUMN — progress + queue
# ─────────────────────────────────────────────────────────────────────────
def _build_right_column(col, app):
    app._ffpkg_queue_meta_var = tk.StringVar(value=_('0 queued'))

    # ── Progress card ──
    prog_card = Card(col, title=_('Build progress'),
                     subtitle=_('Live status appears here once a build starts.'))
    prog_card.pack(fill='x')

    # Staged progress (3 stages: Write Structure / Copy Files / Finalise)
    app._ffpkg_build_stages = StageTracker(
        prog_card.body,
        stages=[_('Write Structure'), _('Copy Files'), _('Finalise')],
        bg=BG2, meta=False)
    app._ffpkg_build_stages.pack(fill='x')

    # Backwards-compat aliases
    app._ffpkg_bar_canvas = app._ffpkg_build_stages.bar_canvas
    app._ffpkg_bar_rect   = app._ffpkg_build_stages.bar_rect
    app._ffpkg_stage_dots = []  # legacy iterator becomes a no-op
    app._ffpkg_current_pct = 0

    # Step + percentage row
    step_row = tk.Frame(prog_card.body, bg=BG2)
    step_row.pack(fill='x', pady=(px(10), 0))
    tk.Label(step_row, textvariable=app._ffpkg_step_var,
             font=FONTS['body_strong'], bg=BG2, fg=C['fg_0'],
             anchor='w').pack(side='left')
    tk.Label(step_row, textvariable=app._ffpkg_pct_var, font=FONTS['body'],
             bg=BG2, fg=C['fg_2'], anchor='e').pack(side='right')

    # ETA row
    eta_row = tk.Frame(prog_card.body, bg=BG2)
    eta_row.pack(fill='x', pady=(px(2), 0))
    tk.Label(eta_row, textvariable=app._ffpkg_eta_var, font=FONTS['caption'],
             bg=BG2, fg=C['fg_3'], anchor='w').pack(side='left')

    # ── Queue card ──
    q_card = Card(col, title=_('Queue'))
    q_card.pack(fill='x', pady=(px(16), 0))
    app._ffpkg_queue_count_var = tk.StringVar(value='0 items')
    tk.Label(q_card.head_actions, textvariable=app._ffpkg_queue_count_var,
             font=FONTS['caption'], bg=BG2, fg=C['fg_3']).pack(
                 side='left', padx=(0, px(10)))
    Button(q_card.head_actions, text=_('Clear'), icon='trash', size='sm',
           variant='subtle', command=app._ffpkg_clear_queue,
           surface=BG2).pack(side='left')

    # Queue list area — scrollable canvas
    q_outer = tk.Frame(q_card.body, bg=BG2)
    q_outer.pack(fill='x')
    Divider(q_outer, color=C['border_1']).pack(fill='x')
    canvas = tk.Canvas(q_outer, bg=BG2, highlightthickness=0,
                       height=px(300))
    sb = XScrollbar(q_outer, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    app._ffpkg_queue_canvas = canvas
    app._ffpkg_queue_frame = tk.Frame(canvas, bg=BG2)
    canvas.create_window((0, 0), window=app._ffpkg_queue_frame,
                         anchor='nw', tags='fqf')
    app._ffpkg_queue_frame.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig('fqf', width=e.width))
    attach_scroll(canvas)

    # ── Output log — HIDDEN.
    # The widget tree is kept alive so the existing _ffpkg_log /
    # _ffpkg_log_box / _ffpkg_toggle_log methods on the App class
    # don't crash, but it's never packed. Output is funnelled to the
    # global OUTPUT LOG drawer at the bottom of the window via a
    # write-through hook installed below.
    _log_hidden = tk.Frame(col, bg=BG1)
    # (intentionally not packed)
    app._ffpkg_log_visible = tk.BooleanVar(value=False)
    app._ffpkg_log_toggle = tk.Label(_log_hidden, text='OUTPUT LOG')
    app._ffpkg_log_body = ConsoleView(_log_hidden, height=10)
    app._ffpkg_log_box = app._ffpkg_log_body.text

    # Install a write-through hook so anything inserted into the
    # hidden log widget is mirrored to the global OUTPUT LOG drawer.
    _install_ffpkg_log_mirror(app)


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _field(parent, label_text, var, on_browse=None, hint=None):
    """Label-above-input field with optional Browse button.

    Returns the block Frame so the legacy code can use it as the
    `before=...` reference for packing the detected strip.
    """
    block = field_block(parent, label_text, var, on_browse=on_browse,
                        browse_text=_('Browse'), hint=hint)
    block.pack_configure(pady=(0, px(12)))
    return block


def _accent_btn(parent, text, command):
    """Secondary action button ('Add to queue')."""
    return Button(parent, text=text, command=command, icon='plus')


def _teal_btn(parent, text, command):
    """Primary action button ('Build All')."""
    return Button(parent, text=text, command=command, variant='primary')


def _success_btn(parent, text, command):
    """Back-compat alias for _teal_btn."""
    return _teal_btn(parent, text, command)


def _install_ffpkg_log_mirror(app):
    """Hook the hidden _ffpkg_log_box Text widget so any insert() call
    is mirrored to the global OUTPUT LOG drawer. Idempotent per process.
    """
    if getattr(app, '_ffpkg_log_mirror_installed', False):
        return
    app._ffpkg_log_mirror_installed = True

    txt = getattr(app, '_ffpkg_log_box', None)
    if txt is None:
        return

    original_insert = txt.insert

    def _wrapped_insert(*args, **kwargs):
        try:
            original_insert(*args, **kwargs)
        except Exception:
            pass
        # Mirror to global log
        try:
            chars = args[1] if len(args) >= 2 else kwargs.get('chars', '')
            if chars and hasattr(app, '_log'):
                line = chars if chars.endswith('\n') else chars + '\n'
                if not line.startswith('[FFPKG]') and \
                        not line.startswith('['):
                    line = '[FFPKG] ' + line
                app._log(line)
                # Auto-open the global drawer so user sees output
                try:
                    app._toggle_log(force_open=True)
                except Exception:
                    pass
        except Exception:
            pass

    txt.insert = _wrapped_insert
