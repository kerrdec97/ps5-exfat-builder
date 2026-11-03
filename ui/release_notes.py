"""
ui/release_notes.py — Data-driven release announcement system.

ONE source of truth for the "What's new" experience. To ship a new release,
edit ONLY the RELEASE dict below (bump `version`, set `title`/`summary`/
`highlights`/`improvements`, update `supporters`). Every surface — the
first-launch modal, the dashboard support card, and the post-success prompt —
reads from this data. No release notes are hardcoded in the UI.

Surfaces provided:
  • dialog_shell / dialog_present / dialog_heading
                                   — the design-system dialog frame (bg_2
                                     body, bg_1 footer band) shared by the
                                     app's own small dialogs.
  • show_release_modal(app)        — the single first-launch modal (Steam/VS
                                     Code style). Shown once per new version.
  • maybe_show_release_modal(app)  — gate: shows the modal only if this
                                     version hasn't been acknowledged yet.
  • build_support_card(parent,app) — permanent dashboard support card widget.
  • note_successful_operation(app, op_label)
                                   — count a successful op and, per a
                                     frequency policy, maybe show a small
                                     post-success support nudge (never every
                                     launch).

All persistence uses app._settings via save_settings(); keys are namespaced
under "release_" / "support_" so they're easy to find and reset.
"""

import os
import time
import webbrowser

import tkinter as tk
from ui.shared.widgets import (Button as XButton, Check as XCheck, Radio as XRadio, TextField as XEntry, Scrollbar as XScrollbar)
from ui.shared.widgets import Badge, Card, Divider, IconLabel, Rounded
from ui.shared.scroll import wheel_units, attach_scroll

from tkinter_theme import COLORS, FONTS, px

# Support / project links (single definition).
KOFI_URL   = 'https://ko-fi.com/deckerr9746220'
REPO_OWNER = 'kerrdec97'
REPO_NAME  = 'ps5-exfat-builder'
RELEASES_URL = 'https://github.com/%s/%s/releases' % (REPO_OWNER, REPO_NAME)


# ─────────────────────────────────────────────────────────────────────────
# THE ONLY THING TO EDIT FOR A NEW RELEASE
# ─────────────────────────────────────────────────────────────────────────
RELEASE = {
    "version": "4.1.0",
    "title": "Farewell Edition",
    "summary": ("The final release of exFAT Image Builder. It restores the "
                "complete v4.0.2 feature set, stops finished images from "
                "being deleted by mistake, makes builds work the same on "
                "every Windows language, and gives the whole app a cleaner, "
                "more consistent look. Thank you for everything."),
    "highlights": [
        "Finished images are never deleted because a status check disagreed \u2014 they are kept and flagged instead",
        "Non-English Windows: copy results are counted on the image itself, no longer read from translated robocopy text",
        "Verification waits for slow-to-appear drives and names any files that are actually missing",
        "Fixed the cause of the PFS \u201cErrno 22 Invalid argument\u201d crash on non-English systems",
    ],
    "improvements": [
        "Stuck OSFMount drivers now time out with a clear message instead of hanging the queue",
        "Sidebar and page titles follow your interface language",
        "Clear explanations for full images and a missing .NET 8 runtime",
        "Backports: the originals ZIP folder is checked before any file changes and earlier backups are never overwritten",
        "Safer in-app updater and a reproducible, verified release build",
        "Refreshed graphite theme with a single accent colour and visible keyboard focus",
    ],
    "supporters": 6,
    "show_on_first_launch": True,
}


# ─────────────────────────────────────────────────────────────────────────
# Persistence helpers
# ─────────────────────────────────────────────────────────────────────────
def _settings(app):
    s = getattr(app, '_settings', None)
    return s if isinstance(s, dict) else {}


def _save(app):
    try:
        from exfat_builder import save_settings
        save_settings(app._settings)
    except Exception:
        pass


def _seen_version(app):
    return _settings(app).get('release_last_seen_version', '')


def _mark_seen(app, version):
    try:
        app._settings['release_last_seen_version'] = version
        _save(app)
    except Exception:
        pass


def should_show_modal(app):
    """True if the current RELEASE hasn't been acknowledged on this machine
    and the release opts in to first-launch display."""
    if not RELEASE.get('show_on_first_launch', True):
        return False
    return _seen_version(app) != RELEASE.get('version', '')


# ─────────────────────────────────────────────────────────────────────────
# Small shared widgets — design-system dialog shell
# ─────────────────────────────────────────────────────────────────────────
C = COLORS
BG1, BG2, BG3 = C['bg_1'], C['bg_2'], C['bg_3']


def _btn(parent, text, primary=False, command=None, accent=None, icon=None,
         variant=None):
    """A design-system button. primary=filled accent, else secondary.
    Footer buttons get a ~96px minimum width (negative ttk width)."""
    v = variant or ('primary' if primary else 'secondary')
    return XButton(parent, text=text, command=command, variant=v, icon=icon,
                   width=-9)


def dialog_shell(app, title, width=480, height=None):
    """Create a modal dialog laid out per the design system: a bg_2 body
    and a bg_1 footer band (hairline on top) for right-aligned buttons.

    Returns (win, body, footer). Call `dialog_present(win, app, w, h)`
    once the content is built to centre, show and grab it."""
    win = tk.Toplevel(app)
    win.withdraw()
    win.title(title)
    win.configure(bg=BG2)
    win.transient(app)
    foot_band = tk.Frame(win, bg=BG1)
    foot_band.pack(side='bottom', fill='x')
    Divider(win, color=C['border_2']).pack(side='bottom', fill='x')
    footer = tk.Frame(foot_band, bg=BG1)
    footer.pack(fill='x', padx=px(20), pady=px(16))
    body = tk.Frame(win, bg=BG2)
    body.pack(fill='both', expand=True)
    win._xb_size = (width, height)
    return win, body, footer


def dialog_present(win, app, width=None, height=None):
    """Size (or fit), centre over the main window, show and grab."""
    try:
        win.update_idletasks()
        w = width or win.winfo_reqwidth()
        h = height or win.winfo_reqheight()
        app.update_idletasks()
        x = app.winfo_rootx() + (app.winfo_width() - w) // 2
        y = app.winfo_rooty() + (app.winfo_height() - h) // 3
        win.geometry('%dx%d+%d+%d' % (w, h, max(0, x), max(0, y)))
    except Exception:
        pass
    win.deiconify()
    try:
        win.grab_set()
    except Exception:
        pass


def dialog_heading(parent, title, icon=None, icon_fg=None, wrap=None):
    """Dialog title row: optional 20px glyph + `title` text."""
    row = tk.Frame(parent, bg=BG2)
    row.pack(fill='x')
    if icon:
        IconLabel(row, icon, 20, icon_fg or C['fg_2'], BG2).pack(
            side='left', padx=(0, px(10)))
    tk.Label(row, text=title, font=FONTS['title'], bg=BG2, fg=C['fg_0'],
             anchor='w', justify='left',
             wraplength=wrap or 0).pack(side='left', fill='x', expand=True)
    return row


def _open(url):
    try:
        webbrowser.open(url)
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────────────
# 1) FIRST-LAUNCH MODAL  (the single popup)
# ─────────────────────────────────────────────────────────────────────────
def maybe_show_release_modal(app):
    """Show the release modal once per new version. Safe no-op otherwise."""
    try:
        if should_show_modal(app):
            show_release_modal(app)
    except Exception:
        pass


def show_release_modal(app, force=False):
    """Render the single, self-contained release modal from RELEASE.

    One window only — no stacked popups, donation integrated inline. On
    dismiss, the version is remembered so it won't show again until a newer
    version ships. `force=True` (e.g. from a menu) shows it regardless.
    """
    r = RELEASE
    ver = r.get('version', '')

    sw, sh = app.winfo_screenwidth(), app.winfo_screenheight()
    W = min(px(600), sw - 80)
    H = min(px(680), sh - 100)
    win, outer, bar = dialog_shell(app, "What's new — v" + ver, W, H)

    def _dismiss():
        if not force:
            _mark_seen(app, ver)
        try:
            win.grab_release()
        except Exception:
            pass
        win.destroy()

    win.protocol('WM_DELETE_WINDOW', _dismiss)

    # Scroll wrapper (small screens still see everything; buttons pinned below)
    canvas = tk.Canvas(outer, bg=BG2, highlightthickness=0)
    vsb = XScrollbar(outer, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=vsb.set)
    vsb.pack(side='right', fill='y', padx=(0, px(4)), pady=px(4))
    canvas.pack(side='left', fill='both', expand=True)
    body = tk.Frame(canvas, bg=BG2)
    cid = canvas.create_window((0, 0), window=body, anchor='nw')
    body.bind('<Configure>',
              lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e: canvas.itemconfig(cid, width=e.width))
    attach_scroll(canvas)

    pad = tk.Frame(body, bg=BG2)
    pad.pack(fill='both', expand=True, padx=px(24), pady=(px(24), px(24)))
    wrap = W - px(80)

    # ── Overline + version ──
    top = tk.Frame(pad, bg=BG2)
    top.pack(fill='x')
    tk.Label(top, text="WHAT'S NEW", font=FONTS['overline'], bg=BG2,
             fg=C['fg_4']).pack(side='left')
    Badge(top, 'v' + ver, 'outline', mono=True, surface=BG2).pack(
        side='left', padx=(px(10), 0))

    # ── Title + summary ──
    tk.Label(pad, text=r.get('title', ''), font=FONTS['title'], bg=BG2,
             fg=C['fg_0'], anchor='w', justify='left',
             wraplength=wrap).pack(fill='x', pady=(px(8), px(6)))
    tk.Label(pad, text=r.get('summary', ''), font=FONTS['body'], bg=BG2,
             fg=C['fg_2'], anchor='w', justify='left',
             wraplength=wrap).pack(fill='x')

    # ── Section renderer ──
    def _section(title, items, icon):
        if not items:
            return
        tk.Label(pad, text=title, font=FONTS['subtitle'], bg=BG2,
                 fg=C['fg_0'], anchor='w').pack(fill='x', pady=(px(24), px(8)))
        for it in items:
            row = tk.Frame(pad, bg=BG2)
            row.pack(fill='x', pady=px(4))
            IconLabel(row, icon, 16, C['fg_3'], BG2).pack(
                side='left', anchor='n', padx=(0, px(10)), pady=(px(1), 0))
            tk.Label(row, text=it, font=FONTS['body'], bg=BG2, fg=C['fg_1'],
                     anchor='w', justify='left',
                     wraplength=wrap - px(30)).pack(side='left', fill='x',
                                                    expand=True)

    _section('Highlights', r.get('highlights', []), 'sparkles')
    _section("What's improved", r.get('improvements', []), 'check')

    # ── Integrated support panel (no separate popup) ──
    sup = Rounded(pad, bg=BG3, border=C['border_3'], radius=8,
                  padding=(px(16), px(14)), surface=BG2)
    sup.pack(fill='x', pady=(px(24), 0))
    IconLabel(sup, 'heart', 16, C['fg_3'], BG3).pack(
        side='left', anchor='n', padx=(0, px(12)), pady=(px(2), 0))
    sup_in = tk.Frame(sup, bg=BG3)
    sup_in.pack(side='left', fill='x', expand=True)
    tk.Label(sup_in, text='Support development', font=FONTS['body_strong'],
             bg=BG3, fg=C['fg_0'], anchor='w').pack(fill='x')
    n_sup = r.get('supporters')
    sup_msg = ("This project is maintained by a single developer. If this "
               "tool has saved you time, consider supporting future updates.")
    if isinstance(n_sup, int) and n_sup > 0:
        sup_msg += "  Thank you to the %d supporters so far." % n_sup
    tk.Label(sup_in, text=sup_msg, font=FONTS['body'], bg=BG3, fg=C['fg_2'],
             anchor='w', justify='left',
             wraplength=wrap - px(60)).pack(fill='x', pady=(px(4), px(8)))
    XButton(sup_in, text='Buy me a coffee', icon='coffee', variant='secondary',
            size='sm', surface=BG3,
            command=lambda: _open(KOFI_URL)).pack(anchor='w')

    # ── Pinned footer (always visible, not scrolled) ──
    _btn(bar, _t(app, 'View Full Changelog'), variant='subtle',
         icon='file_text', command=lambda: _open_changelog(app)
         ).pack(side='left')
    start = _btn(bar, _t(app, 'Start Building'), primary=True,
                 command=_dismiss)
    start.pack(side='right')
    _btn(bar, _t(app, 'Support Development'), icon='heart',
         command=lambda: _open(KOFI_URL)).pack(side='right', padx=(0, px(8)))

    win.bind('<Escape>', lambda e: _dismiss())
    win.bind('<Return>', lambda e: _dismiss())
    dialog_present(win, app, W, H)
    try:
        start.focus_set()
    except Exception:
        pass
    return win


def _t(app, s):
    """Optional translation passthrough (uses app._ if present)."""
    try:
        fn = getattr(app, '_tr', None) or globals().get('_')
        if callable(fn):
            return fn(s)
    except Exception:
        pass
    return s


def _open_changelog(app):
    """Open the on-disk CHANGELOG.md if present, else the releases page."""
    path = None
    try:
        getter = getattr(app, '_changelog_path', None)
        if callable(getter):
            path = getter()
    except Exception:
        path = None
    if path and os.path.isfile(path):
        try:
            if os.name == 'nt':
                os.startfile(path)  # noqa
                return
        except Exception:
            pass
        try:
            _open('file:///' + path.replace('\\', '/'))
            return
        except Exception:
            pass
    _open(RELEASES_URL)


# ─────────────────────────────────────────────────────────────────────────
# 2) DASHBOARD SUPPORT CARD  (permanent; replaces aggressive popups)
# ─────────────────────────────────────────────────────────────────────────
def build_support_card(parent, app):
    """Return a permanent 'Support Development' dashboard card frame.

    Shows version, and (best-effort) Downloads / Releases pulled live from
    GitHub in the background. Falls back to the local version count if the
    network is unavailable. Pure display + one button.
    """
    card = Card(parent, title='Support development', padding=16)
    tk.Label(card.head_actions, text='v' + RELEASE.get('version', ''),
             font=FONTS['caption'], bg=BG2, fg=C['fg_4']).pack()
    inner = card.body

    # Live stat line (filled async)
    stat_var = tk.StringVar(value='Downloads: …   Releases: …')
    tk.Label(inner, textvariable=stat_var, font=FONTS['body'], bg=BG2,
             fg=C['fg_1'], anchor='w').pack(fill='x')

    tk.Label(inner,
             text=('Donations are appreciated but never required — this '
                   'tool will always be free and open source.'),
             font=FONTS['caption'], bg=BG2, fg=C['fg_3'],
             anchor='w', justify='left', wraplength=px(320)).pack(
                 fill='x', pady=(px(4), px(12)))

    _btn(inner, _t(app, 'Support Project'), primary=True, icon='heart',
         command=lambda: _open(KOFI_URL)).pack(anchor='w')

    _load_repo_stats(app, stat_var)
    return card


def _load_repo_stats(app, stat_var):
    """Best-effort background fetch of release count + total download count
    from the GitHub API. Updates stat_var on the UI thread. Never raises."""
    import threading

    def _work():
        downloads = None
        releases = None
        try:
            import json
            import urllib.request
            url = ('https://api.github.com/repos/%s/%s/releases'
                   '?per_page=100' % (REPO_OWNER, REPO_NAME))
            req = urllib.request.Request(
                url, headers={'User-Agent': 'exfat-builder'})
            with urllib.request.urlopen(req, timeout=6) as resp:
                data = json.loads(resp.read().decode('utf-8', 'replace'))
            if isinstance(data, list):
                releases = len(data)
                total = 0
                for rel in data:
                    for asset in (rel.get('assets') or []):
                        try:
                            total += int(asset.get('download_count', 0))
                        except Exception:
                            pass
                downloads = total
        except Exception:
            pass

        def _apply():
            if downloads is None and releases is None:
                # Network unavailable — show what we know locally.
                stat_var.set('Releases: %s' % RELEASE.get('version', ''))
            else:
                d = ('{:,}'.format(downloads) if isinstance(downloads, int)
                     else '\u2014')
                rc = (str(releases) if isinstance(releases, int) else '\u2014')
                stat_var.set('Downloads: %s    Releases: %s' % (d, rc))
        try:
            app.after(0, _apply)
        except Exception:
            pass

    threading.Thread(target=_work, daemon=True).start()


# ─────────────────────────────────────────────────────────────────────────
# 3) POST-SUCCESS PROMPT  (occasional; never every launch)
# ─────────────────────────────────────────────────────────────────────────
# Frequency policy: show after every N successful operations OR once every
# M days, whichever comes first — and never more than once per session.
_OP_INTERVAL = 10          # every 10 successful operations
_DAY_INTERVAL = 30 * 86400  # or once every 30 days
_shown_this_session = {'v': False}


def note_successful_operation(app, op_label='Operation'):
    """Increment the success counter and, per the frequency policy, maybe
    show the small post-success support dialog. Call this once from each
    operation's success handler (build/extract/convert)."""
    try:
        s = _settings(app)
        count = int(s.get('support_op_count', 0)) + 1
        app._settings['support_op_count'] = count
        last_ts = float(s.get('support_prompt_last_ts', 0) or 0)
        now = time.time()

        due_by_count = (count % _OP_INTERVAL == 0)
        due_by_time = (last_ts > 0 and (now - last_ts) >= _DAY_INTERVAL)
        # First-ever prompt: wait until the first interval, not the very
        # first operation, so brand-new users aren't nagged immediately.
        first_ok = (last_ts == 0 and count >= _OP_INTERVAL)

        _save(app)

        if _shown_this_session['v']:
            return
        if due_by_count or due_by_time or first_ok:
            _show_success_prompt(app, op_label)
    except Exception:
        pass


def _show_success_prompt(app, op_label):
    _shown_this_session['v'] = True
    try:
        app._settings['support_prompt_last_ts'] = time.time()
        _save(app)
    except Exception:
        pass

    W = px(440)
    win, body, btns = dialog_shell(app, op_label + ' Complete', W)
    win.resizable(False, False)

    def _close():
        try:
            win.grab_release()
        except Exception:
            pass
        win.destroy()

    win.protocol('WM_DELETE_WINDOW', _close)

    pad = tk.Frame(body, bg=BG2)
    pad.pack(fill='both', expand=True, padx=px(24), pady=(px(22), px(22)))
    dialog_heading(pad, op_label + ' Complete', icon='success',
                   icon_fg=C['success'], wrap=W - px(90))
    tk.Label(pad, text=_success_line(op_label), font=FONTS['body'],
             bg=BG2, fg=C['fg_2'], anchor='w', justify='left',
             wraplength=W - px(48)).pack(fill='x', pady=(px(8), px(16)))

    Divider(pad, color=C['border_1']).pack(fill='x')
    tk.Label(pad, text='Enjoying exFAT Image Builder?',
             font=FONTS['body_strong'], bg=BG2, fg=C['fg_0'],
             anchor='w').pack(fill='x', pady=(px(14), px(2)))
    tk.Label(pad, text='Support helps fund future development.',
             font=FONTS['body'], bg=BG2, fg=C['fg_2'], anchor='w',
             justify='left', wraplength=W - px(48)).pack(fill='x')

    support = _btn(btns, _t(app, 'Support Development'), primary=True,
                   icon='heart', command=lambda: (_open(KOFI_URL), _close()))
    support.pack(side='right')
    _btn(btns, _t(app, 'Close'), primary=False,
         command=_close).pack(side='right', padx=(0, px(8)))

    win.bind('<Escape>', lambda e: _close())
    win.bind('<Return>', lambda e: _close())
    dialog_present(win, app, W, None)
    try:
        support.focus_set()
    except Exception:
        pass
    return win


def _success_line(op_label):
    low = (op_label or '').lower()
    if 'build' in low:
        return 'Image created successfully.'
    if 'extract' in low:
        return 'Extraction completed successfully.'
    if 'convert' in low:
        return 'Conversion completed successfully.'
    return 'Operation completed successfully.'
