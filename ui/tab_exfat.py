"""
ui/tab_exfat.py — exFAT image build tab (legacy per-format Build sub-tab).

Design-system layout (sibling of the unified Build screen):

    Page header  "Build exFAT image"
    (i) Tip: Size / Cluster / Sector / Thread settings live in Advanced
    ┌ Source ──────────────────────────────┐ ┌ Build progress ─────────────┐
    │ drop strip                           │ │ ✓ Mount ▶ Format ○ Copy ○ … │
    │ detected game (cover · title · size) │ │ ▬▬▬▬▬▬▬▬▬▬▬▬───────── 63%   │
    │ Game root folder   [_______][Browse] │ │ step · ETA · queue ETA      │
    │ Add multiple folders at once         │ └─────────────────────────────┘
    │ Output directory   [_______][Browse] │ ┌ Build queue ─[Load][Save][…]┐
    │ Output filename    (read-only)       │ │ rows: status · title · size │
    ├──────────────────────────────────────┤ │       actions               │
    │ Force dismount   Pause  Add  [Build] │ └─────────────────────────────┘
    └──────────────────────────────────────┘
    status line

All callbacks (`app._add_to_queue`, `app._run_queue`, etc.) are unchanged —
this rewrite is UI-only. Every attribute the rest of the app reads on `app`
is preserved (see `Compatibility` at the bottom).
"""

import os
import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from tkinter import ttk

from tkinter_theme import COLORS, FONTS, px

# Star-import remains for the legacy color constants (BG, ACCENT, ...) that
# show up in callbacks executed against `app`.
from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _

from ui.shared.page_head import field_block
from ui.shared.scroll import attach_scroll
from ui.shared.widgets import (Button, Card, Divider, InfoBar, PageHeader,
                               Rounded)
from ui.tab_extract import (scroll_page, page_columns, DropStrip,
                            DetectedStrip, StageTracker, caption)

C = COLORS
BG1, BG2 = COLORS['bg_1'], COLORS['bg_2']


# ─────────────────────────────────────────────────────────────────────────
# Helpers (kept for any caller that still imports them)
# ─────────────────────────────────────────────────────────────────────────
_KIND_VARIANT = {'primary': 'primary', 'success': 'primary',
                 'success_done': 'secondary', 'warn': 'secondary',
                 'danger': 'danger', 'ghost': 'subtle'}


def _make_themed_button(parent, text, command, kind='primary',
                        icon=None, font_size=10, padx=14, pady=7,
                        state='normal'):
    """Design-system button for a legacy `kind` (primary/success/warn/
    danger/ghost). `icon` may be a glyph or an icon name."""
    label = (icon + '  ' + text) if icon else text
    return Button(parent, text=label, command=command, state=state,
                  variant=_KIND_VARIANT.get(kind, 'secondary'),
                  size='sm' if font_size <= 9 else 'md')


def _info_banner(parent, text, on_click=None):
    """Inline info bar; the whole bar is clickable when on_click is set."""
    bar = InfoBar(parent, kind='info', text=text)
    if on_click:
        def _bind(w):
            try:
                w.bind('<Button-1>', lambda e: on_click(), add='+')
                w.configure(cursor='hand2')
            except Exception:
                pass
            for c in w.winfo_children():
                _bind(c)
        _bind(bar)
    return bar


# ─────────────────────────────────────────────────────────────────────────
# Main entry — build the exFAT tab into `parent`.
# ─────────────────────────────────────────────────────────────────────────
def build_exfat_tab(parent, app):
    """Build the exFAT tab body into `parent`: page header, tip, then the
    Source form (left) and the progress + queue cards (right)."""
    parent.configure(bg=BG1)

    body = tk.Frame(parent, bg=BG1)
    body.pack(fill='both', expand=True)

    # Bottom status line — packed FIRST with side='bottom' so it pins
    # regardless of how tall the page above gets.
    status_row = tk.Frame(body, bg=BG1)
    status_row.pack(side='bottom', fill='x', padx=px(24), pady=(px(6), px(8)))
    app.status_lbl = tk.Label(status_row, textvariable=app.status_text,
                              font=FONTS['body'], bg=BG1,
                              fg=C['success_hi'], anchor='w')
    app.status_lbl.pack(side='left')
    Divider(body, color=C['border_2']).pack(side='bottom', fill='x')

    page_host = tk.Frame(body, bg=BG1)
    page_host.pack(fill='both', expand=True)
    page = scroll_page(page_host)

    head = PageHeader(page, _('Build exFAT image'),
                      _('Pick a game folder, set the output, and queue the build.'))
    head.pack(fill='x', padx=px(24), pady=(px(20), px(16)))

    banner = _info_banner(page,
        'Tip: Recommended Size, Cluster, Sector & Thread settings live in '
        'the Advanced tab. Click here to jump there.',
        on_click=lambda: app._switch_tab('advanced'))
    banner.pack(fill='x', padx=px(24), pady=(0, px(16)))

    left, right = page_columns(page)
    _build_left_column(left, app)
    _build_right_column(right, app)


def _build_left_column(col, app):
    """Source card: drop strip → detected game → game folder → multi-add
    link → output directory → output filename, with the action footer."""
    card = Card(col, title='Source',
                subtitle='The game dump folder that contains eboot.bin.',
                actions=True)
    card.pack(fill='x')
    body = card.body

    # ── Compact drop strip ──
    dropzone = _CompactDropZone(body,
                                on_drop=lambda p: _on_dropzone_drop(app, p),
                                on_click=app._browse_game)
    dropzone.pack(fill='x')

    # ── Detected game info slot ──
    detected_slot = tk.Frame(body, bg=BG2)
    detected_slot.pack(fill='x', pady=(px(12), 0))
    app._detected_strip = DetectedStrip(detected_slot)

    # Backwards-compat aliases for the legacy detection callbacks.
    app._info_frame     = app._detected_strip
    app._cover_label    = app._detected_strip.cover_label
    app._info_title_var = app._detected_strip.title_var
    app._info_id_var    = app._detected_strip.id_var
    app._info_ver_var   = app._detected_strip.ver_var
    app._info_size_var  = app._detected_strip.size_var
    app._cover_image    = None  # the existing code maintains the image ref

    # ── Game folder field ──
    fld_game = field_block(body, 'Game root folder', var=app.game_folder,
                           on_browse=app._browse_game,
                           hint='must contain eboot.bin')
    fld_game.pack_configure(pady=0)

    # ── "Add multiple folders at once" link button ──
    multi_row = tk.Frame(body, bg=BG2)
    multi_row.pack(fill='x', pady=(px(6), 0))
    Button(multi_row, text='Add multiple folders at once', icon='folder_plus',
           variant='link', command=app._browse_multi_games,
           surface=BG2).pack(side='left')

    # ── Output directory field ──
    fld_out = field_block(body, 'Output directory', var=app.output_dir,
                          on_browse=app._browse_output)
    fld_out.pack_configure(pady=(px(6), 0))

    # ── Output filename row (read-only preview + auto/no-sfo labels) ──
    fn_block = tk.Frame(body, bg=BG2)
    fn_block.pack(fill='x', pady=(px(12), 0))
    fn_label_row = tk.Frame(fn_block, bg=BG2)
    fn_label_row.pack(fill='x')
    tk.Label(fn_label_row, text='Output filename', font=FONTS['body'],
             bg=BG2, fg=C['fg_2'], anchor='w').pack(side='left')
    app._auto_label = tk.Label(fn_label_row, text='auto-detected',
                               font=FONTS['caption'], bg=BG2,
                               fg=C['success_hi'], anchor='w', padx=px(8))
    app._auto_label.pack(side='left')
    app._auto_label.pack_forget()
    app._no_sfo_label = tk.Label(fn_label_row,
                                 text='no metadata found '
                                      '— will use folder name',
                                 font=FONTS['caption'], bg=BG2,
                                 fg=C['warn_hi'], anchor='w', padx=px(8))
    app._no_sfo_label.pack(side='left')
    app._no_sfo_label.pack_forget()

    fn_input = Rounded(fn_block, bg=C['bg_1'], border=C['border_2'],
                       radius=4, padding=(px(12), px(7)), surface=BG2)
    fn_input.pack(fill='x', pady=(px(4), 0))
    app._filename_preview = tk.Label(fn_input, textvariable=app.output_name,
                                     font=FONTS['code'], bg=C['bg_1'],
                                     fg=C['fg_1'], anchor='w')
    app._filename_preview.pack(fill='x')
    caption(body, 'Read-only — built from the detected title, ID and '
                  'version.').pack(fill='x', pady=(px(4), 0))

    # ── Action footer — Force Dismount left, build actions right ──
    acts = card.actions
    Button(acts, text='Force Dismount', icon='eject', variant='subtle',
           command=app._force_dismount_all, surface=BG2).pack(side='left')

    app.build_btn = Button(acts, text='Build All', icon='play',
                           variant='primary', command=app._run_queue,
                           surface=BG2)
    app.build_btn.pack(side='right')
    Button(acts, text='Add to Queue', icon='plus', command=app._add_to_queue,
           surface=BG2).pack(side='right', padx=(0, px(6)))
    app._pause_btn = Button(acts, text='Pause', icon='pause',
                            command=app._toggle_pause_queue, state='disabled',
                            surface=BG2)
    app._pause_btn.pack(side='right', padx=(0, px(6)))


class _CompactDropZone(DropStrip):
    """One-row drop target above the Browse fields (drag-drop + click to
    browse in one slim inset strip)."""

    def __init__(self, parent, on_drop=None, on_click=None):
        super().__init__(parent, 'Drop a game folder here, or click to browse',
                         on_drop=on_drop, on_click=on_click,
                         hint='must contain eboot.bin', icon='folder_in')


def _build_right_column(col, app):
    """Progress card stacked above the queue card."""
    # ── Build progress card ──
    prog_card = Card(col, title='Build progress',
                     subtitle='Live status appears here once a build starts.')
    prog_card.pack(fill='x')

    app._build_stages = StageTracker(prog_card.body,
                                     stages=['Mount', 'Format',
                                             'Copy files', 'Verify'],
                                     bg=BG2, meta=False)
    app._build_stages.pack(fill='x')

    # Backwards-compat aliases for the legacy callbacks
    # (`_update_bar_visual`, `_on_bar_resize`, `_activate_dot`).
    app._bar_canvas = app._build_stages.bar_canvas
    app._bar_rect   = app._build_stages.bar_rect
    app._stage_dots = []
    # Keep the legacy width cache (`app._bar_width`) in sync with the bar.
    try:
        app._bar_canvas.bind('<Configure>', app._on_bar_resize, add='+')
    except Exception:
        pass

    meta_row = tk.Frame(prog_card.body, bg=BG2)
    meta_row.pack(fill='x', pady=(px(10), 0))
    tk.Label(meta_row, textvariable=app._step_label_var,
             font=FONTS['body_strong'], bg=BG2, fg=C['fg_0'],
             anchor='w').pack(side='left')
    tk.Label(meta_row, textvariable=app._pct_var, font=FONTS['body'],
             bg=BG2, fg=C['fg_2'], anchor='e').pack(side='right')

    eta_row = tk.Frame(prog_card.body, bg=BG2)
    eta_row.pack(fill='x', pady=(px(2), 0))
    tk.Label(eta_row, textvariable=app._eta_var, font=FONTS['caption'],
             bg=BG2, fg=C['fg_3'], anchor='w').pack(side='left')
    tk.Label(eta_row, textvariable=app._queue_eta_var, font=FONTS['caption'],
             bg=BG2, fg=C['fg_3'], anchor='e').pack(side='right')

    # ── Build queue card ──
    q_card = Card(col, title='Build queue')
    q_card.pack(fill='x', pady=(px(16), 0))
    app._queue_count_var = tk.StringVar(value='0 items')
    tk.Label(q_card.head_actions, textvariable=app._queue_count_var,
             font=FONTS['caption'], bg=BG2, fg=C['fg_3']).pack(
                 side='left', padx=(0, px(10)))
    _qbtn(q_card.head_actions, '\U0001f4c2  Load',
          command=app._queue_load).pack(side='left', padx=(0, px(4)))
    _qbtn(q_card.head_actions, '\U0001f4be  Save',
          command=app._queue_save).pack(side='left', padx=(0, px(4)))
    _qbtn(q_card.head_actions, '\U0001f5d1  Clear', danger=True,
          command=app._clear_queue).pack(side='left')

    # Drop hint above the list. Same DnD logic as the legacy code — when
    # tkinterdnd2 is unavailable the hint says so.
    hint_row = tk.Frame(q_card.body, bg=BG2)
    hint_row.pack(fill='x', pady=(0, px(8)))
    dnd_hint = tk.Label(hint_row,
                        text='Drop game folders here to add to queue',
                        font=FONTS['caption'], bg=BG2, fg=C['fg_3'],
                        anchor='w')
    dnd_hint.pack(side='left')

    # Scrollable queue list (rows are appended by app._render_queue).
    q_outer = tk.Frame(q_card.body, bg=BG2)
    q_outer.pack(fill='x')
    Divider(q_outer, color=C['border_1']).pack(fill='x')
    canvas = tk.Canvas(q_outer, bg=BG2, highlightthickness=0,
                       height=px(300))
    sb = XScrollbar(q_outer, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=sb.set)
    sb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)

    app._queue_canvas = canvas
    app._queue_frame = tk.Frame(canvas, bg=BG2)
    canvas.create_window((0, 0), window=app._queue_frame,
                         anchor='nw', tags='qf')
    app._queue_frame.bind('<Configure>', lambda e:
        canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e:
        canvas.itemconfig('qf', width=e.width))

    # ── Drag & drop for game folders onto the queue ──
    def _hint(text, fg):
        try:
            dnd_hint.config(text=text, fg=fg)
        except Exception:
            pass

    def _drop_to_queue(event):
        try:
            paths = app.tk.splitlist(event.data)
            for raw in paths:
                p = raw.strip('{}').strip('"')
                if os.path.isdir(p):
                    app._game_var.set(p)
                    app._on_game_browse_done(p)
                    app._add_to_queue()
                    _hint('Added to queue', C['success_hi'])
                    app.after(2000, lambda: _hint(
                        'Drop game folders here to add to queue', C['fg_3']))
        except Exception as e:
            app._log('[DROP] Error: %s\n' % e)

    try:
        app.drop_target_register('DND_Files')
        app.dnd_bind('<<Drop>>', _drop_to_queue)
    except Exception:
        _hint('Install tkinterdnd2 to enable drag & drop', C['fg_4'])
    try:
        canvas.drop_target_register('DND_Files')
        canvas.dnd_bind('<<Drop>>', getattr(app, '_on_queue_drop',
                                            _drop_to_queue))
    except Exception:
        pass


def _qbtn(parent, text, command, danger=False):
    """Small (28px) header button for the queue card. A leading glyph in
    `text` becomes the icon."""
    return Button(parent, text=text, command=command, size='sm',
                  variant='subtle' if danger else 'secondary')


def _on_dropzone_drop(app, path):
    """Called when a folder is dropped into the drop strip.

    Just sets the game folder and triggers the existing detection logic.
    No queue auto-add — the user clicks "+ Add to Queue" explicitly.
    """
    if not os.path.isdir(path):
        return
    try:
        app._game_var.set(path)
        app._on_game_browse_done(path)
    except Exception as e:
        try:
            app._log('[DROP] Error: %s\n' % e)
        except Exception:
            pass


# ─────────────────────────────────────────────────────────────────────────
# Compatibility — attributes preserved on `app` for the rest of the app.
# ─────────────────────────────────────────────────────────────────────────
# After build_exfat_tab() returns, every `app.*` attribute the rest of the
# codebase expects is in place:
#
#   app._info_frame        — the DetectedStrip; pack()/pack_forget()
#   app._cover_label       — its cover tile (.config(image=…, text=…))
#   app._info_title_var / _info_id_var / _info_ver_var / _info_size_var
#   app._cover_image       — None initially; the detector reuses the slot
#   app._auto_label        — "auto-detected" label, hidden initially
#   app._no_sfo_label      — "no metadata found" label, hidden initially
#   app._filename_preview  — read-only Label showing the resolved filename
#   app._build_stages      — StageTracker (StagedProgressBar API)
#   app._bar_canvas        — CompatBar the legacy `_update_bar_visual`
#                            drives with coords()/itemconfig()
#   app._bar_rect          — the bar item id for those calls
#   app._stage_dots        — empty list (legacy iteration is a no-op)
#   app._queue_count_var   — header count StringVar
#   app._queue_canvas      — scroll canvas for queue rows
#   app._queue_frame       — frame inside the canvas where rows are appended
#   app.build_btn          — primary "Build All" button
#   app._pause_btn         — pause toggle button (starts disabled)
#   app.status_lbl         — bottom status line
