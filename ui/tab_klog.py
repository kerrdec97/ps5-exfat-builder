"""
ui/tab_klog.py — Klog Monitor (live PS5 kernel log viewer).

Step 4 (v2.0.6): refactored against klog-tab-redesign-standalone.html.

Layout (top to bottom, single column, full width):

    [▣]  Klog Monitor                          [● CONNECTED · IP · uptime]
         Stream the PS5 kernel log live…       [1,284 info · 17 warn · 3 err]
    ┌─ Connection bar ──────────────────────────────────────────────────┐
    │ IP [192.168.0.173]  Port [3232]  [▶Connect] [■Disconnect] [⏸Pause]│
    │                                              Default port: 3232 ··│
    └────────────────────────────────────────────────────────────────────┘
    [🗑 Clear] [💾 Export] (Auto-scroll) (Word wrap) (Timestamps)  1,304 ln
    ┌─ Filter ───────────────────────────────────────────────────────────┐
    │ 🔍 [Filter — type to highlight…]                          [Clear]  │
    └────────────────────────────────────────────────────────────────────┘
    ┌─ ConsoleView (terminal-green on near-black) ───────────────────────┐
    │ 12:04:01  INFO  [hen] goldHEN v2.4b18.4 booting…                   │
    │ 12:04:03  WARN  [exfat] volume label mismatch — recovered          │
    │ 12:04:09  ERR   [npdrm] license_check failed (rc=0x80024021)       │
    │ ...                                                                │
    └────────────────────────────────────────────────────────────────────┘

Backwards compat: every `app._klog_*` attribute the existing callbacks rely
on is preserved with the same name and same widget API:
    _klog_box, _klog_lines, _klog_count_var, _klog_filter_var,
    _klog_autoscroll, _klog_word_wrap, _klog_timestamps, _klog_paused,
    _klog_status_var, _klog_status_lbl,
    _klog_connect_btn, _klog_stop_btn, _klog_pause_btn

Level filter pills (ALL/ERR/WARN/INFO/DBG) from the mock are NOT included
in this iteration — they would require modifying _klog_add_line and
_klog_apply_filter, which is out of scope per the brief's "don't change
callbacks" rule. Can be added in a follow-up.
"""

import tkinter as tk

from tkinter_theme import COLORS, FONTS

# Star-import provides the legacy theme constants (BG, ACCENT, ...) plus
# the i18n function `_`. Step 5 cleanup will trim this.
from exfat_builder import *  # noqa: F401,F403
from exfat_builder import _

from ui.shared.cards import StatPill
from ui.shared.dashboard import StatCard
from ui.shared.forms import SegmentedToggle
from ui.shared.log_view import ConsoleView


def build_klog_tab(parent, app):
    """Build the redesigned Klog Monitor tab body into `parent`."""
    parent.configure(bg=COLORS['bg_1'])

    # ── State ──
    app._klog_ip_var      = tk.StringVar(
        value=app._settings.get('klog_ip', app._settings.get('ftp_ip', '')))
    app._klog_port_var    = tk.StringVar(
        value=str(app._settings.get('klog_port', 3232)))
    app._klog_running     = False
    app._klog_socket      = [None]
    app._klog_lines       = []
    app._klog_filter_var  = tk.StringVar()
    app._klog_autoscroll  = tk.BooleanVar(value=True)
    app._klog_word_wrap   = tk.BooleanVar(value=True)
    app._klog_timestamps  = tk.BooleanVar(value=True)
    app._klog_paused      = tk.BooleanVar(value=False)

    # The whole tab uses a vertical pack stack. The ConsoleView at the
    # bottom takes expand=True so it grows with the window.
    body = tk.Frame(parent, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, padx=20, pady=12)

    _build_page_head(body, app)
    _build_stats_strip(body, app)
    _build_conn_bar(body, app)
    _build_toolbar(body, app)
    _build_filter_row(body, app)
    _build_console(body, app)


# ─────────────────────────────────────────────────────────────────────────
# Page head — green badge + title + connection status pill + stats strip
# ─────────────────────────────────────────────────────────────────────────
def _build_page_head(body, app):
    head = tk.Frame(body, bg=COLORS['bg_1'])
    head.pack(fill='x', pady=(0, 10))

    # Green gradient-ish badge tile (▣ glyph, success-on-bg-darker)
    badge = tk.Label(head, text='\u25a3',
                     bg=COLORS['success'], fg='#001a05',
                     font=(FONTS['body'][0], 16, 'bold'),
                     width=2, height=1, padx=4, pady=0,
                     highlightbackground='#00b32d',
                     highlightthickness=1)
    badge.pack(side='left', padx=(0, 12))

    # Titles column (expands to push status pill + stats to the right)
    title_col = tk.Frame(head, bg=COLORS['bg_1'])
    title_col.pack(side='left', fill='x', expand=True)
    tk.Label(title_col, text=_('Klog Monitor'),
             font=(FONTS['h2'][0], 14, 'bold'),
             bg=COLORS['bg_1'], fg=COLORS['fg_0'],
             anchor='w'
             ).pack(fill='x')
    tk.Label(title_col,
             text=_('Stream the PS5 kernel log live — filter, color, export.'),
             font=FONTS['meta'],
             bg=COLORS['bg_1'], fg=COLORS['fg_4'],
             anchor='w'
             ).pack(fill='x', pady=(1, 0))

    # ── Connection status pill ──
    # The mock has a green pulsing pill ("CONNECTED · 192.168.0.173 · 00:04:21").
    # We use a static green pill and a dim grey one for disconnected.
    # tk doesn't do CSS animations; the dot is a static green/grey circle.
    status_pill = tk.Frame(head, bg=COLORS['success_bg'],
                           highlightbackground=COLORS['success'],
                           highlightthickness=1)
    status_pill.pack(side='right')
    pill_inner = tk.Frame(status_pill, bg=COLORS['success_bg'])
    pill_inner.pack(padx=10, pady=4)

    # Status dot (small filled circle via fixed-size Frame)
    app._klog_status_dot = tk.Frame(pill_inner, bg=COLORS['fg_5'],
                                    width=8, height=8)
    app._klog_status_dot.pack(side='left', padx=(0, 6))
    app._klog_status_dot.pack_propagate(False)

    app._klog_status_var = tk.StringVar(value=_('Disconnected'))
    app._klog_status_lbl = tk.Label(pill_inner,
                                    textvariable=app._klog_status_var,
                                    font=(FONTS['mono_sm'][0], 10, 'bold'),
                                    bg=COLORS['success_bg'],
                                    fg=COLORS['fg_4'])
    app._klog_status_lbl.pack(side='left')


# ─────────────────────────────────────────────────────────────────────────
# Stats strip — Build-style StatCards (v3.6.0 PS5 pass)
#
# Live counters derived on the presentation side: a 1-second after()
# poll walks NEW entries in `app._klog_lines` (the buffer the existing
# stream callback already maintains) and classifies them with the
# existing `app._klog_get_tag`. The Klog socket / parse / add-line
# implementation is untouched.
# ─────────────────────────────────────────────────────────────────────────
def _build_stats_strip(body, app):
    strip = tk.Frame(body, bg=COLORS['bg_1'])
    strip.pack(fill='x', pady=(0, 10))

    def _cell(icon, caption, status=None):
        sc = StatCard(strip, icon=icon, value='0', caption=caption,
                      status=status)
        sc.pack(side='left', fill='both', expand=True, padx=(0, 8))
        return sc

    app._klog_stat_total = _cell('\U0001f4dc', _('Total Lines'))
    app._klog_stat_rate  = _cell('\u23f1',     _('Lines / sec'))
    app._klog_stat_warn  = _cell('\u26a0',     _('Warnings'), status='warn')
    app._klog_stat_err   = _cell('\u2715',     _('Errors'),   status='error')

    conn = StatCard(strip, icon='\U0001f50c', value=_('OFFLINE'),
                    caption=_('Connection'))
    conn.pack(side='left', fill='both', expand=True)
    app._klog_stat_conn = conn
    # Keep the old attribute name alive (it used to point at a StatPill;
    # nothing outside this module reads it, but stale references in user
    # scripts shouldn't explode).
    app._klog_stat_info = app._klog_stat_total

    # Mirror the connection state the existing callbacks write
    def _sync_conn(*_a):
        try:
            conn.set_value(app._klog_status_var.get())
        except Exception:
            pass
    app._klog_status_var.trace_add('write', _sync_conn)
    _sync_conn()

    # ── 1s poll: classify new lines, compute rate ──
    state = {'idx': 0, 'warn': 0, 'err': 0, 'last_total': 0}

    def _tick():
        try:
            if not strip.winfo_exists():
                return
        except Exception:
            return
        try:
            lines = app._klog_lines
            total = len(lines)
            if total < state['idx']:
                # Buffer was cleared (Clear button) — reset counters
                state['idx'] = 0
                state['warn'] = 0
                state['err'] = 0
                state['last_total'] = 0
            for i in range(state['idx'], total):
                try:
                    tag = app._klog_get_tag(lines[i])
                except Exception:
                    tag = 'info'
                if tag == 'error':
                    state['err'] += 1
                elif tag == 'warning':
                    state['warn'] += 1
            state['idx'] = total

            rate = max(0, total - state['last_total'])
            state['last_total'] = total

            app._klog_stat_total.set_value('{:,}'.format(total))
            app._klog_stat_rate.set_value(str(rate))
            app._klog_stat_warn.set_value('{:,}'.format(state['warn']))
            app._klog_stat_err.set_value('{:,}'.format(state['err']))
        except Exception:
            pass
        try:
            strip.after(1000, _tick)
        except Exception:
            pass
    strip.after(1000, _tick)


# ─────────────────────────────────────────────────────────────────────────
# Connection bar — IP / Port inputs + Connect / Disconnect / Pause buttons
# ─────────────────────────────────────────────────────────────────────────
def _build_conn_bar(body, app):
    bar = tk.Frame(body, bg=COLORS['bg_2'],
                   highlightbackground=COLORS['border_2'],
                   highlightthickness=1)
    bar.pack(fill='x', pady=(0, 8))

    inner = tk.Frame(bar, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=12, pady=8)

    # IP label + input
    tk.Label(inner, text='IP',
             font=(FONTS['label'][0], 10, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(0, 6))
    _dark_entry(inner, app._klog_ip_var, width=18).pack(side='left',
                                                       padx=(0, 12))

    # Port label + input
    tk.Label(inner, text='Port',
             font=(FONTS['label'][0], 10, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(0, 6))
    _dark_entry(inner, app._klog_port_var, width=8).pack(side='left',
                                                        padx=(0, 12))

    # Connect / Disconnect / Pause
    app._klog_connect_btn = _btn(inner, '\u25b6  Connect',
                                 command=app._klog_connect, kind='success')
    app._klog_connect_btn.pack(side='left')

    app._klog_stop_btn = _btn(inner, '\u25a0  Disconnect',
                              command=app._klog_disconnect, kind='danger',
                              state='disabled')
    app._klog_stop_btn.pack(side='left', padx=(6, 0))

    app._klog_pause_btn = _btn(inner, '\u23f8  Pause',
                               command=app._klog_toggle_pause, kind='warn',
                               state='disabled')
    app._klog_pause_btn.pack(side='left', padx=(6, 0))

    # Right-aligned hint
    tk.Label(inner, text=_('Default port: 3232  ·  TCP socket'),
             font=FONTS['meta'],
             bg=COLORS['bg_2'], fg=COLORS['fg_5']
             ).pack(side='right')


# ─────────────────────────────────────────────────────────────────────────
# Toolbar — Clear / Export buttons + segmented toggles + line counter
# ─────────────────────────────────────────────────────────────────────────
def _build_toolbar(body, app):
    bar = tk.Frame(body, bg=COLORS['bg_1'])
    bar.pack(fill='x', pady=(0, 6))

    _btn(bar, '\U0001f5d1  Clear',
         command=app._klog_clear, kind='ghost').pack(side='left')

    _btn(bar, '\U0001f4be  Export log',
         command=app._klog_export, kind='ghost').pack(side='left',
                                                       padx=(6, 0))

    # Segmented toggles
    SegmentedToggle(bar, _('Auto-scroll'),
                    var=app._klog_autoscroll
                    ).pack(side='left', padx=(12, 0))
    SegmentedToggle(bar, _('Word wrap'),
                    var=app._klog_word_wrap,
                    on_change=app._klog_toggle_wrap
                    ).pack(side='left', padx=(6, 0))
    SegmentedToggle(bar, _('Timestamps'),
                    var=app._klog_timestamps
                    ).pack(side='left', padx=(6, 0))

    # Line counter (right-aligned)
    app._klog_count_var = tk.StringVar(value='0 lines')
    tk.Label(bar, textvariable=app._klog_count_var,
             font=FONTS['mono_sm'],
             bg=COLORS['bg_1'], fg=COLORS['success_hi']
             ).pack(side='right')


# ─────────────────────────────────────────────────────────────────────────
# Filter row — search input with regex hint + Clear button
# ─────────────────────────────────────────────────────────────────────────
def _build_filter_row(body, app):
    row = tk.Frame(body, bg=COLORS['bg_2'],
                   highlightbackground=COLORS['border_2'],
                   highlightthickness=1)
    row.pack(fill='x', pady=(0, 8))

    inner = tk.Frame(row, bg=COLORS['bg_2'])
    inner.pack(fill='x', padx=12, pady=6)

    tk.Label(inner, text='\U0001f50d',
             font=(FONTS['body'][0], 11),
             bg=COLORS['bg_2'], fg=COLORS['fg_4']
             ).pack(side='left', padx=(0, 8))

    # The filter input itself — dark theme variant (the design system rule
    # "fields are light-on-dark" doesn't apply to inline filter inputs,
    # which use the dark style per the mock).
    filter_entry = tk.Entry(inner, textvariable=app._klog_filter_var,
                            font=FONTS['mono_sm'],
                            bg=COLORS['bg_2'], fg=COLORS['fg_1'],
                            insertbackground=COLORS['accent'],
                            selectbackground=COLORS['accent'],
                            selectforeground=COLORS['fg_0'],
                            relief='flat', bd=0,
                            highlightbackground=COLORS['bg_2'],
                            highlightthickness=0)
    filter_entry.pack(side='left', fill='x', expand=True, padx=(0, 8))

    # Watch the var → re-render the log on every keystroke
    app._klog_filter_var.trace('w', lambda *a: app._klog_apply_filter())

    # Clear-filter button (small, ghost-style)
    tk.Button(inner, text=_('Clear'),
              font=FONTS['meta'],
              bg=COLORS['bg_3'], fg=COLORS['fg_4'],
              activebackground=COLORS['bg_4'], activeforeground=COLORS['fg_1'],
              relief='flat', bd=0, padx=8, pady=2,
              cursor='hand2',
              command=lambda: app._klog_filter_var.set('')
              ).pack(side='left')


# ─────────────────────────────────────────────────────────────────────────
# Console view — terminal-styled tk.Text in a ConsoleView wrapper
# ─────────────────────────────────────────────────────────────────────────
def _build_console(body, app):
    cv = ConsoleView(body)
    cv.pack(fill='both', expand=True, pady=(0, 4))

    # Backwards-compat: legacy callbacks read/write `app._klog_box` directly.
    # Expose the inner Text widget under that name. The 'error', 'warning',
    # 'info', 'debug', and 'highlight' tags are pre-configured by ConsoleView.
    app._klog_box = cv.text
    # Keep a handle on the wrapper too, in case future code wants the
    # convenience methods (.append, .clear, .set_word_wrap).
    app._klog_console = cv


# ─────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────
def _dark_entry(parent, var, width=20):
    """A dark-themed entry box (bg_4 fill, border_3 hairline).

    Differs from `LabeledField`'s light entry — used in inline toolbars
    where the design uses dark inputs.
    """
    f = tk.Frame(parent, bg=COLORS['bg_4'],
                 highlightbackground=COLORS['border_3'],
                 highlightthickness=1)
    e = tk.Entry(f, textvariable=var,
                 font=FONTS['mono_sm'],
                 bg=COLORS['bg_4'], fg=COLORS['fg_1'],
                 insertbackground=COLORS['accent'],
                 selectbackground=COLORS['accent'],
                 selectforeground=COLORS['fg_0'],
                 relief='flat', bd=0,
                 width=width)
    e.pack(padx=8, pady=5)
    return f


def _btn(parent, text, command, kind='primary', state='normal'):
    """Themed tk.Button — same kind→colors mapping as in tab_exfat.

    Kept local to tab_klog for now; if this gets reused 3+ times across
    tabs, promote to ui/shared/forms.py as ThemedButton in a later step.
    """
    schemes = {
        'success': (COLORS['success'], COLORS['fg_0'],
                    COLORS['success_hi'], COLORS['fg_0']),
        'danger':  (COLORS['danger'],  COLORS['fg_0'],
                    COLORS['danger_hi'], COLORS['fg_0']),
        'warn':    (COLORS['warn'],    '#1a0e00',
                    COLORS['warn_hi'], '#1a0e00'),
        'ghost':   (COLORS['bg_3'],    COLORS['fg_2'],
                    COLORS['bg_4'],    COLORS['fg_1']),
    }
    bg, fg, abg, afg = schemes.get(kind, schemes['ghost'])
    btn = tk.Button(parent, text=text,
                    font=(FONTS['button'][0], 9, 'bold'),
                    bg=bg, fg=fg,
                    activebackground=abg, activeforeground=afg,
                    relief='flat', bd=0,
                    padx=10, pady=4,
                    cursor='hand2', state=state,
                    command=command)
    if kind == 'ghost':
        btn.configure(highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
    return btn
