"""
ui/tab_ffpkg.py — ffpkg image builder tab (UFS2Tool, .NET 8 dependency).

Step 9 (v2.1.1): full refactor against preview/ffpkg-tab-redesign.html.

Layout:

    ┌─ LEFT (Build form) ──────────┬─ RIGHT (Queue & progress) ─────────┐
    │ [📦 banner — UFS2 / .ffpkg]  │ ┌─ progress card ─────────────────┐ │
    │ [● .NET 8 detected v8.0.11]  │ │ Building Returnal · 62%         │ │
    │ ┌─ drop zone ──────────────┐ │ │ [████████████─────]              │ │
    │ │ 📂 Drop game folder…     │ │ │ 35.2 / 56.8 GB · 412 MB/s · ETA │ │
    │ └─────────────────────────┘ │ └─────────────────────────────────┘ │
    │ ┌─ DetectedGameStrip(teal)─┐ │ ┌─ Queue card ────────────────────┐ │
    │ │ 🎮 Returnal              │ │ │ Queue          3 items · 195 GB │ │
    │ │ CUSA-22871 v2.10 SDK 4   │ │ │ ┌────────────────────────────┐ │ │
    │ └─────────────────────────┘ │ │ │ 1 Returnal     56.8GB ●Build│ │ │
    │ Output directory             │ │ │ 2 Demon's Souls 62.4GB Queued│ │ │
    │ [...........] [Browse]      │ │ │ 3 Ratchet      76.5GB Queued│ │ │
    │ Output filename · auto       │ │ │ ✓ Astros       4.2GB  Done  │ │ │
    │ [📄 game.ffpkg]              │ │ └────────────────────────────┘ │ │
    │ ┌─ UFS2 parameters card ───┐ │ └─────────────────────────────────┘ │
    │ │ 🔒 customise in Advanced │ │                                      │
    │ │ -O 2  UFS version    2   │ │                                      │
    │ │ -b    Block size     32k │ │                                      │
    │ │ -f    Fragment size  4k  │ │                                      │
    │ │ -m    Min free %     8   │ │                                      │
    │ │ -S    Sector size    512 🔒│                                      │
    │ │ -i    Inode density  4k  │ │                                      │
    │ └────────────────────────┘ │ │                                      │
    │ [+ Add to queue][🔨 Build All]                                      │
    └─────────────────────────────┴─────────────────────────────────────┘

Backwards compat: every `app._ffpkg_*` attribute the existing 50+ callbacks
read or write is preserved with the same name. Worker logic untouched.
The `_ffpkg_activate_dot` callback got a 1-line forwarder addition (same
pattern as `_activate_dot` in step 3).
"""

import os
import tkinter as tk

from tkinter_theme import COLORS, FONTS

from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _, save_settings

from ui.shared.cards import Card, DetectedGameStrip, StatusPill, StatPill
from ui.shared.forms import DropZone
from ui.shared.progress import StagedProgressBar
from ui.shared.log_view import ConsoleView
from ui.shared.page_head import (
    make_themed_button, info_banner, page_head)


def build_ffpkg_tab(parent, app):
    """Build the redesigned ffpkg tab body into `parent`."""
    parent.configure(bg=COLORS['bg_1'])

    # ── State ──
    app._ffpkg_game_folder = tk.StringVar()
    app._ffpkg_output_dir  = tk.StringVar(
        value=app._settings.get('ffpkg_output_dir', ''))
    app._ffpkg_output_name = tk.StringVar(value='game.ffpkg')
    app._ffpkg_step_var    = tk.StringVar(value='')
    app._ffpkg_pct_var     = tk.StringVar(value='')
    app._ffpkg_eta_var     = tk.StringVar(value='')

    # 2-column workspace
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True)

    # Configure columns 50/50 — caller can resize the window to redistribute
    body.grid_columnconfigure(0, weight=1, uniform='ffpkg', minsize=400)
    body.grid_columnconfigure(1, weight=1, uniform='ffpkg', minsize=380)
    body.grid_rowconfigure(0, weight=1)

    _build_left_column(body, app)
    _build_right_column(body, app)


# ─────────────────────────────────────────────────────────────────────────
# LEFT COLUMN — build form
# ─────────────────────────────────────────────────────────────────────────
def _build_left_column(body, app):
    left = tk.Frame(body, bg=COLORS['bg_1'])
    left.grid(row=0, column=0, sticky='nsew')

    # 1px right separator (matches mock's `border-right`)
    tk.Frame(body, bg=COLORS['border_2'], width=1
             ).grid(row=0, column=0, sticky='nse')

    # ── Column header (Build-tab spacious style) ──
    head = page_head(left, '\U0001f4e6',
                     _('Build ffpkg image'),
                     _('Pick a game folder and queue the build.'))
    head.pack(fill='x', padx=24, pady=(14, 12))
    # Mode indicator on the right
    tk.Label(head, text=_('UFS2 · S=512 fixed'),
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(side='right', padx=(8, 0))

    # ── Scrollable body ──
    wrap = tk.Frame(left, bg=COLORS['bg_1'])
    wrap.pack(fill='both', expand=True)

    # Pin action buttons to the bottom of `wrap` BEFORE the scrollable
    # canvas takes over the remaining vertical space. This way the
    # Add-to-queue / Build All row stays visible even when the global
    # OUTPUT LOG opens and squeezes the available height.
    bottom_actions = tk.Frame(wrap, bg=COLORS['bg_1'])
    bottom_actions.pack(side='bottom', fill='x', padx=24, pady=(6, 12))

    cv = tk.Canvas(wrap, bg=COLORS['bg_1'], highlightthickness=0)
    sb = tk.Scrollbar(wrap, command=cv.yview,
                      bg=COLORS['bg_2'], troughcolor=COLORS['bg_1'])
    cv.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    cv.pack(side='left', fill='both', expand=True)
    inner_tag = 'ffpkg_left_inner'
    inner = tk.Frame(cv, bg=COLORS['bg_1'])
    cv.create_window((0, 0), window=inner, anchor='nw', tags=inner_tag)

    # Clamp scrollregion to canvas height when content fits, so the user
    # can't scroll past the bottom of the form into empty space.
    def _ffpkg_left_update(e=None):
        cv.update_idletasks()
        bbox = cv.bbox('all')
        if not bbox:
            return
        canvas_h = cv.winfo_height()
        content_h = bbox[3] - bbox[1]
        if content_h <= canvas_h:
            cv.configure(scrollregion=(0, 0, bbox[2], canvas_h))
        else:
            cv.configure(scrollregion=bbox)
    inner.bind('<Configure>', _ffpkg_left_update)
    cv.bind('<Configure>',
        lambda e, c=cv: (c.itemconfig(inner_tag, width=e.width),
                          _ffpkg_left_update()))
    cv.bind('<MouseWheel>',
        lambda e, c=cv: c.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

    pad = tk.Frame(inner, bg=COLORS['bg_1'])
    pad.pack(fill='x', padx=24, pady=18)

    # ── Info banner — teal-tinted ──
    banner = tk.Frame(pad, bg=COLORS['teal_bg'],
                      highlightbackground=COLORS['teal_lo'],
                      highlightthickness=1)
    banner.pack(fill='x', pady=(0, 12))
    inner_b = tk.Frame(banner, bg=COLORS['teal_bg'])
    inner_b.pack(fill='x', padx=12, pady=10)
    tk.Label(inner_b, text='\U0001f4e6',
             font=(FONTS['body'][0], 12),
             bg=COLORS['teal_bg'], fg=COLORS['teal']
             ).pack(side='left', padx=(0, 10))
    tk.Label(inner_b,
             text=_('Creates UFS2 (.ffpkg) images using UFS2Tool with sector '
                    'size locked at 512 bytes for Windows compatibility.'),
             font=FONTS['body'],
             bg=COLORS['teal_bg'], fg=COLORS['teal_hi'],
             anchor='w', justify='left', wraplength=460
             ).pack(side='left', fill='x', expand=True)

    # ── .NET runtime check pill ──
    dotnet = tk.Frame(pad, bg=COLORS['success_bg'],
                      highlightbackground=COLORS['success_hi'],
                      highlightthickness=1)
    dotnet.pack(fill='x', pady=(0, 12))
    dn_inner = tk.Frame(dotnet, bg=COLORS['success_bg'])
    dn_inner.pack(fill='x', padx=12, pady=8)
    # Status dot
    dot = tk.Frame(dn_inner, bg=COLORS['success'],
                   width=8, height=8)
    dot.pack(side='left', padx=(0, 8))
    dot.pack_propagate(False)
    app._ffpkg_dotnet_status_var = tk.StringVar(
        value=_('.NET 8 runtime — click to verify'))
    tk.Label(dn_inner, textvariable=app._ffpkg_dotnet_status_var,
             font=FONTS['body'],
             bg=COLORS['success_bg'], fg=COLORS['success_hi'],
             anchor='w'
             ).pack(side='left', fill='x', expand=True)
    # Right-side button to manually re-check
    tk.Button(dn_inner, text=_('Check'),
              font=(FONTS['button'][0], 9),
              bg=COLORS['success_bg'], fg=COLORS['success_hi'],
              activebackground=COLORS['bg_3'],
              activeforeground=COLORS['fg_0'],
              relief='flat', bd=0,
              padx=10, pady=2,
              cursor='hand2',
              command=app._ffpkg_check_dotnet
              ).pack(side='right')

    # ── Drop zone ──
    drop = DropZone(pad,
                    on_drop=lambda p: _on_drop(app, p),
                    on_click=app._ffpkg_browse_game,
                    hint=_('Folder must contain eboot.bin'))
    drop.pack(fill='x', pady=(0, 12))

    # ── Detected game strip (teal-tinted, kind='teal') ──
    # Lives in a slot frame so legacy pack/forget logic keeps working.
    slot = tk.Frame(pad, bg=COLORS['bg_1'])
    slot.pack(fill='x')
    strip = DetectedGameStrip(slot, kind='teal')
    # NOT packed yet — the legacy `_ffpkg_detect_game` packs it via the
    # `_ffpkg_info_frame` alias below.

    # Backwards-compat aliases — every name the legacy code expects:
    app._ffpkg_info_frame     = strip
    app._ffpkg_cover_label    = strip.cover_label
    app._ffpkg_info_title_var = strip.title_var
    app._ffpkg_info_id_var    = strip.id_var       # NEW — populated by patched callback
    app._ffpkg_info_ver_var   = strip.ver_var      # NEW — populated by patched callback
    app._ffpkg_info_size_var  = strip.size_var
    app._ffpkg_cover_image    = None

    # ── Output directory field ──
    app._ffpkg_output_dir_frame = _field(pad,
        _('Output directory'),
        var=app._ffpkg_output_dir,
        on_browse=app._ffpkg_browse_output)

    # ── Output filename preview (read-only, light bg) ──
    fn_block = tk.Frame(pad, bg=COLORS['bg_1'])
    fn_block.pack(fill='x', pady=(0, 12))

    fn_label_row = tk.Frame(fn_block, bg=COLORS['bg_1'])
    fn_label_row.pack(fill='x')
    tk.Label(fn_label_row, text=_('Output filename'),
             font=FONTS['label'],
             bg=COLORS['bg_1'], fg=COLORS['fg_3'],
             anchor='w'
             ).pack(side='left')
    app._ffpkg_auto_label = tk.Label(fn_label_row,
                                     text='  \u2022 ' + _('auto-detected'),
                                     font=FONTS['meta'],
                                     bg=COLORS['bg_1'], fg=COLORS['success_hi'],
                                     anchor='w')
    app._ffpkg_auto_label.pack(side='left')
    app._ffpkg_auto_label.pack_forget()

    # Read-only preview using field_bg (light-on-dark, design system rule)
    fn_input = tk.Frame(fn_block, bg=COLORS['field_bg'],
                        highlightbackground=COLORS['border_3'],
                        highlightthickness=1)
    fn_input.pack(fill='x', pady=(6, 0))
    app._ffpkg_filename_preview = tk.Label(fn_input,
                                           textvariable=app._ffpkg_output_name,
                                           font=FONTS['mono_sm'],
                                           bg=COLORS['field_bg'],
                                           fg=COLORS['field_fg'],
                                           anchor='w', padx=10, pady=8)
    app._ffpkg_filename_preview.pack(fill='x')

    # ── UFS2 parameters card (read-only, sourced from advanced settings) ──
    _build_params_card(pad, app)

    # ── Action buttons ──
    # Packed into the pinned bottom_actions frame (above), NOT into the
    # scrollable inner. This keeps them visible regardless of how much
    # vertical space the OUTPUT LOG eats from the body.
    actions = bottom_actions

    _accent_btn(actions, '+  ' + _('Add to queue'),
                command=app._ffpkg_add_to_queue
                ).pack(side='left', fill='x', expand=True, padx=(0, 6))

    app._ffpkg_build_btn = _teal_btn(actions,
        '\U0001f528  ' + _('Build All'),
        command=app._ffpkg_run_queue)
    app._ffpkg_build_btn.pack(side='left')


def _build_params_card(parent, app):
    """Read-only UFS2 parameters card. Values sourced live from
    `_adv_ffpkg_*` vars; the -S row is locked at 512."""
    card = Card(parent, title=_('UFS2 parameters'),
                subtitle=_('Customise these in the Advanced tab.'),
                icon='\u2699')
    card.pack(fill='x', pady=(0, 12))

    # Row helper — 3-column grid: [-key] [description] [value]
    def _param_row(key, desc, value_var, locked=False):
        row = tk.Frame(card.body, bg=COLORS['bg_2'])
        row.pack(fill='x', pady=2)
        # 3 columns
        row.grid_columnconfigure(0, weight=0, minsize=80)
        row.grid_columnconfigure(1, weight=1)
        row.grid_columnconfigure(2, weight=0, minsize=70)

        tk.Label(row, text=key,
                 font=(FONTS['mono_sm'][0], 10, 'bold'),
                 bg=COLORS['bg_2'], fg=COLORS['teal'],
                 anchor='w'
                 ).grid(row=0, column=0, sticky='w')
        tk.Label(row, text=desc,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_4'],
                 anchor='w'
                 ).grid(row=0, column=1, sticky='w')

        val_text = ' \U0001f512' if locked else ''
        if isinstance(value_var, tk.StringVar):
            val_lbl = tk.Label(row, textvariable=value_var,
                               font=(FONTS['mono'][0], 10, 'bold'),
                               bg=COLORS['bg_2'],
                               fg=COLORS['success_hi'] if locked else COLORS['fg_1'],
                               anchor='e')
        else:
            val_lbl = tk.Label(row, text=str(value_var) + val_text,
                               font=(FONTS['mono'][0], 10, 'bold'),
                               bg=COLORS['bg_2'],
                               fg=COLORS['success_hi'] if locked else COLORS['fg_1'],
                               anchor='e')
        val_lbl.grid(row=0, column=2, sticky='e')

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
    """Handler for the drop zone — sets game folder and triggers detection."""
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
def _build_right_column(body, app):
    right = tk.Frame(body, bg=COLORS['bg_1'])
    right.grid(row=0, column=1, sticky='nsew')

    # ── Column header (Build-tab spacious style) ──
    head = page_head(right, '\U0001f4ca',
                     _('Queue & build progress'),
                     _('Live status and build queue.'))
    head.pack(fill='x', padx=24, pady=(14, 12))
    app._ffpkg_queue_meta_var = tk.StringVar(value=_('0 queued'))
    tk.Label(head, textvariable=app._ffpkg_queue_meta_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4']
             ).pack(side='right', padx=(8, 0))

    # ── Body ──
    pad = tk.Frame(right, bg=COLORS['bg_1'])
    pad.pack(fill='both', expand=True, padx=24, pady=18)

    # ── Progress card ──
    prog_card = Card(pad,
                     title=_('Build progress'),
                     subtitle=_('Live status appears here once a build starts.'),
                     icon='\U0001f4ca')
    prog_card.pack(fill='x', pady=(0, 14))

    # Step + percentage row
    step_row = tk.Frame(prog_card.body, bg=COLORS['bg_2'])
    step_row.pack(fill='x', pady=(0, 6))
    tk.Label(step_row, textvariable=app._ffpkg_step_var,
             font=FONTS['body_b'],
             bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
             ).pack(side='left')
    tk.Label(step_row, textvariable=app._ffpkg_pct_var,
             font=(FONTS['mono'][0], 10, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['teal'], anchor='e'
             ).pack(side='right')

    # Staged progress bar (3 stages: Write Structure / Copy Files / Finalise)
    app._ffpkg_build_stages = StagedProgressBar(
        prog_card.body,
        stages=[_('Write Structure'), _('Copy Files'), _('Finalise')],
        bg=COLORS['bg_2'])
    app._ffpkg_build_stages.pack(fill='x', pady=(2, 6))

    # Backwards-compat aliases
    app._ffpkg_bar_canvas = app._ffpkg_build_stages.bar_canvas
    app._ffpkg_bar_rect   = app._ffpkg_build_stages.bar_rect
    app._ffpkg_stage_dots = []  # legacy iterator becomes a no-op
    app._ffpkg_current_pct = 0

    # ETA row
    eta_row = tk.Frame(prog_card.body, bg=COLORS['bg_2'])
    eta_row.pack(fill='x')
    tk.Label(eta_row, textvariable=app._ffpkg_eta_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_2'], fg=COLORS['fg_4'], anchor='w'
             ).pack(side='left')

    # ── Queue card ──
    q_card = Card(pad, bg=COLORS['bg_2'])
    q_card.pack(fill='both', expand=True)

    # Header inside queue card
    q_head = tk.Frame(q_card.body, bg=COLORS['bg_2'])
    q_head.pack(fill='x')

    head_left = tk.Frame(q_head, bg=COLORS['bg_2'])
    head_left.pack(side='left')
    tk.Label(head_left, text=_('Queue'),
             font=(FONTS['body'][0], 11, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_0']
             ).pack(side='left')

    app._ffpkg_queue_count_var = tk.StringVar(value='0 items')
    tk.Label(head_left, textvariable=app._ffpkg_queue_count_var,
             font=(FONTS['body'][0], 9, 'bold'),
             bg=COLORS['teal_bg'], fg=COLORS['teal_hi'],
             padx=8, pady=2
             ).pack(side='left', padx=(10, 0))

    head_right = tk.Frame(q_head, bg=COLORS['bg_2'])
    head_right.pack(side='right')
    tk.Button(head_right, text='\U0001f5d1  ' + _('Clear'),
              font=FONTS['meta'],
              bg=COLORS['bg_3'], fg=COLORS['fg_3'],
              activebackground=COLORS['bg_4'],
              activeforeground=COLORS['danger_hi'],
              relief='flat', bd=0,
              padx=10, pady=5,
              cursor='hand2',
              highlightbackground=COLORS['border_2'],
              highlightthickness=1,
              command=app._ffpkg_clear_queue
              ).pack(side='left')

    # Queue list area — scrollable canvas
    q_outer = tk.Frame(q_card.body, bg=COLORS['bg_3'],
                       highlightbackground=COLORS['border_2'],
                       highlightthickness=1)
    q_outer.pack(fill='both', expand=True, pady=(10, 0))

    canvas = tk.Canvas(q_outer, bg=COLORS['bg_3'],
                       highlightthickness=0, height=200)
    sb = tk.Scrollbar(q_outer, orient='vertical', command=canvas.yview,
                      bg=COLORS['bg_4'], troughcolor=COLORS['bg_2'])
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    app._ffpkg_queue_canvas = canvas
    app._ffpkg_queue_frame = tk.Frame(canvas, bg=COLORS['bg_3'])
    canvas.create_window((0, 0), window=app._ffpkg_queue_frame,
                         anchor='nw', tags='fqf')
    app._ffpkg_queue_frame.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig('fqf', width=e.width))
    canvas.bind('<MouseWheel>', lambda e:
        canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

    # ── Output log — HIDDEN.
    # The widget tree is kept alive so the existing _ffpkg_log /
    # _ffpkg_log_box / _ffpkg_toggle_log methods on the App class
    # don't crash, but it's never packed. Output is funnelled to the
    # global OUTPUT LOG drawer at the bottom of the window via a
    # write-through hook installed below.
    _log_hidden = tk.Frame(pad, bg=COLORS['bg_1'])
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

    Returns the row Frame so the legacy code can use it as the
    `before=...` reference for packing the detected strip.
    """
    block = tk.Frame(parent, bg=COLORS['bg_1'])
    block.pack(fill='x', pady=(0, 12))

    lbl_row = tk.Frame(block, bg=COLORS['bg_1'])
    lbl_row.pack(fill='x')
    tk.Label(lbl_row, text=label_text,
             font=FONTS['label'],
             bg=COLORS['bg_1'], fg=COLORS['fg_3'],
             anchor='w'
             ).pack(side='left')
    if hint:
        tk.Label(lbl_row, text='  \u2014  ' + hint,
                 font=FONTS['meta'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_5'],
                 anchor='w'
                 ).pack(side='left')

    input_wrap = tk.Frame(block, bg=COLORS['field_bg'],
                          highlightbackground=COLORS['border_3'],
                          highlightthickness=1)
    input_wrap.pack(fill='x', pady=(6, 0))

    tk.Entry(input_wrap, textvariable=var,
             font=FONTS['mono_sm'],
             bg=COLORS['field_bg'], fg=COLORS['field_fg'],
             insertbackground=COLORS['field_fg'],
             selectbackground=COLORS['accent'],
             selectforeground=COLORS['fg_0'],
             relief='flat', bd=6
             ).pack(side='left', fill='x', expand=True)

    if on_browse:
        tk.Button(input_wrap, text=_('Browse'),
                  font=FONTS['button'],
                  bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                  activebackground=COLORS['bg_5'],
                  activeforeground=COLORS['accent'],
                  relief='flat', bd=0,
                  padx=14, pady=6,
                  cursor='hand2',
                  command=on_browse
                  ).pack(side='right')

    return block


def _accent_btn(parent, text, command):
    """Accent (brand-purple) action button — main CTA in ffpkg flows.
    Used for 'Add to queue', which is the primary action on this tab."""
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 10, 'bold'),
                     bg=COLORS['accent'], fg=COLORS['fg_0'],
                     activebackground=COLORS['accent_hi'],
                     activeforeground=COLORS['fg_0'],
                     relief='flat', bd=0,
                     padx=16, pady=8,
                     cursor='hand2',
                     command=command)


def _teal_btn(parent, text, command):
    """Muted teal action button — secondary forward action.
    Used for 'Build All' — recedes next to the purple primary button
    instead of competing with it like the old mint-green did."""
    return tk.Button(parent, text=text,
                     font=(FONTS['button'][0], 10, 'bold'),
                     bg=COLORS['teal'], fg=COLORS['fg_0'],
                     activebackground=COLORS['teal_hi'],
                     activeforeground=COLORS['fg_0'],
                     relief='flat', bd=0,
                     padx=16, pady=8,
                     cursor='hand2',
                     command=command)


def _success_btn(parent, text, command):
    """Back-compat alias for _teal_btn. Old callers in third-party
    forks may still reach for _success_btn; both produce the same
    muted-teal button now. The bright mint green has been retired
    for action buttons app-wide — it's reserved for status dots."""
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
