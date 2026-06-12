"""
ui/release_notes.py — Data-driven release announcement system.

ONE source of truth for the "What's new" experience. To ship a new release,
edit ONLY the RELEASE dict below (bump `version`, set `title`/`summary`/
`highlights`/`improvements`, update `supporters`). Every surface — the
first-launch modal, the dashboard support card, and the post-success prompt —
reads from this data. No release notes are hardcoded in the UI.

Surfaces provided:
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

from tkinter_theme import COLORS, FONTS

# Support / project links (single definition).
KOFI_URL   = 'https://ko-fi.com/deckerr9746220'
REPO_OWNER = 'kerrdec97'
REPO_NAME  = 'ps5-exfat-builder'
RELEASES_URL = 'https://github.com/%s/%s/releases' % (REPO_OWNER, REPO_NAME)


# ─────────────────────────────────────────────────────────────────────────
# THE ONLY THING TO EDIT FOR A NEW RELEASE
# ─────────────────────────────────────────────────────────────────────────
RELEASE = {
    "version": "3.6.4",
    "title": "Faster PFS Builds & Honest Progress",
    "summary": ("A reliability-focused update: dump \u2192 .ffpfsc builds now "
                "stage the heavy intermediate image on your fast temp drive, "
                "PFS unpack shows live progress, and several reported build "
                "and extraction issues are fixed."),
    "highlights": [
        "Dump \u2192 .ffpfsc stages the intermediate image on the temp drive (much faster on SSD-temp / HDD-output setups)",
        "PFS unpack (.ffpfs / .ffpfsc) now shows live progress, speed and ETA",
        "Free-space preflight before OSFMount \u2014 clear errors instead of a bare exit code",
        "Cross-drive final-output moves are verified and crash-safe",
    ],
    "improvements": [
        "Multithread (/MT) extraction progress now advances correctly",
        "Library now lists uncompressed .ffpfs images too",
        "Language Apply rebuilds the whole UI (translation coverage still expanding)",
        "Cleaner Build screen: advanced perf options tucked away, dead/confusing settings removed",
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
# Small shared widgets
# ─────────────────────────────────────────────────────────────────────────
def _btn(parent, text, primary=False, command=None, accent=None):
    """A flat themed button. primary=filled accent, else subtle."""
    if primary:
        bg = accent or COLORS['accent']
        fg = '#ffffff'
        abg = COLORS.get('accent_pressed', bg)
    else:
        bg = COLORS['bg_4']
        fg = COLORS['fg_2']
        abg = COLORS['bg_5']
    b = tk.Button(parent, text=text, command=command,
                  font=FONTS['button'], bg=bg, fg=fg,
                  activebackground=abg, activeforeground='#ffffff',
                  relief='flat', bd=0, padx=16, pady=9, cursor='hand2')
    return b


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

    win = tk.Toplevel(app)
    win.title("What's new \u2014 v" + ver)
    win.configure(bg=COLORS['bg_1'])
    win.transient(app)
    try:
        win.grab_set()
    except Exception:
        pass

    sw, sh = app.winfo_screenwidth(), app.winfo_screenheight()
    W = min(680, sw - 80)
    H = min(720, sh - 100)
    try:
        app.update_idletasks()
        x = app.winfo_x() + (app.winfo_width() - W) // 2
        y = app.winfo_y() + (app.winfo_height() - H) // 2
        win.geometry('%dx%d+%d+%d' % (W, H, max(0, x), max(0, y)))
    except Exception:
        win.geometry('%dx%d' % (W, H))

    def _dismiss():
        if not force:
            _mark_seen(app, ver)
        try:
            win.grab_release()
        except Exception:
            pass
        win.destroy()

    win.protocol('WM_DELETE_WINDOW', _dismiss)

    # Accent top bar
    tk.Frame(win, bg=COLORS['accent'], height=4).pack(fill='x')

    # Scroll wrapper (small screens still see everything; buttons pinned below)
    outer = tk.Frame(win, bg=COLORS['bg_1'])
    outer.pack(fill='both', expand=True)
    canvas = tk.Canvas(outer, bg=COLORS['bg_1'], highlightthickness=0)
    vsb = tk.Scrollbar(outer, orient='vertical', command=canvas.yview)
    canvas.configure(yscrollcommand=vsb.set)
    vsb.pack(side='right', fill='y')
    canvas.pack(side='left', fill='both', expand=True)
    body = tk.Frame(canvas, bg=COLORS['bg_1'])
    cid = canvas.create_window((0, 0), window=body, anchor='nw')
    body.bind('<Configure>',
              lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
    canvas.bind('<Configure>', lambda e: canvas.itemconfig(cid, width=e.width))
    canvas.bind('<MouseWheel>',
                lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'))

    pad = tk.Frame(body, bg=COLORS['bg_1'])
    pad.pack(fill='both', expand=True, padx=30, pady=(22, 18))

    # ── Eyebrow + version pill ──
    top = tk.Frame(pad, bg=COLORS['bg_1'])
    top.pack(fill='x')
    tk.Label(top, text="WHAT'S NEW",
             font=FONTS['eyebrow'], bg=COLORS['bg_1'],
             fg=COLORS['accent_hi']).pack(side='left')
    pill = tk.Frame(top, bg=COLORS['accent_15'])
    pill.pack(side='right')
    tk.Label(pill, text='  v' + ver + '  ', font=FONTS['body_b'],
             bg=COLORS['accent_15'], fg=COLORS['accent_hi']).pack()

    # ── Title + summary ──
    tk.Label(pad, text=r.get('title', ''), font=FONTS['h1'],
             bg=COLORS['bg_1'], fg=COLORS['fg_0'], anchor='w',
             justify='left', wraplength=W - 80).pack(fill='x', pady=(12, 4))
    tk.Label(pad, text=r.get('summary', ''), font=FONTS['body'],
             bg=COLORS['bg_1'], fg=COLORS['fg_3'], anchor='w',
             justify='left', wraplength=W - 80).pack(fill='x')

    # ── Section renderer ──
    def _section(title, items, dot_color, glyph):
        if not items:
            return
        tk.Frame(pad, bg=COLORS['border_2'], height=1).pack(
            fill='x', pady=(18, 14))
        tk.Label(pad, text=title, font=FONTS['h3'],
                 bg=COLORS['bg_1'], fg=COLORS['fg_1'],
                 anchor='w').pack(fill='x', pady=(0, 8))
        for it in items:
            row = tk.Frame(pad, bg=COLORS['bg_1'])
            row.pack(fill='x', pady=2)
            tk.Label(row, text=glyph, font=FONTS['body_b'],
                     bg=COLORS['bg_1'], fg=dot_color,
                     width=2, anchor='w').pack(side='left')
            tk.Label(row, text=it, font=FONTS['body'],
                     bg=COLORS['bg_1'], fg=COLORS['fg_2'], anchor='w',
                     justify='left', wraplength=W - 130).pack(
                         side='left', fill='x', expand=True)

    _section('Highlights', r.get('highlights', []),
             COLORS['accent_hi'], '\u2737')          # ✷
    _section("What's improved", r.get('improvements', []),
             COLORS['teal'], '\u2713')               # ✓

    # ── Integrated support section (no separate popup) ──
    tk.Frame(pad, bg=COLORS['border_2'], height=1).pack(fill='x', pady=(18, 14))
    sup = tk.Frame(pad, bg=COLORS['bg_3'])
    sup.pack(fill='x')
    sup_in = tk.Frame(sup, bg=COLORS['bg_3'])
    sup_in.pack(fill='x', padx=16, pady=14)
    tk.Label(sup_in, text='\u2764  Support Development',
             font=FONTS['body_b'], bg=COLORS['bg_3'],
             fg=COLORS['fg_0'], anchor='w').pack(fill='x')
    n_sup = r.get('supporters')
    sup_msg = ("This project is maintained by a single developer. If this "
               "tool has saved you time, consider supporting future updates.")
    if isinstance(n_sup, int) and n_sup > 0:
        sup_msg += "  Thank you to the %d supporters so far." % n_sup
    tk.Label(sup_in, text=sup_msg, font=FONTS['label'],
             bg=COLORS['bg_3'], fg=COLORS['fg_3'], anchor='w',
             justify='left', wraplength=W - 110).pack(fill='x', pady=(4, 10))
    _btn(sup_in, '\u2615  Buy Me A Coffee', primary=True,
         command=lambda: _open(KOFI_URL)).pack(anchor='w')

    # ── Pinned button bar (always visible, not scrolled) ──
    bar = tk.Frame(win, bg=COLORS['bg_2'])
    bar.pack(fill='x', side='bottom')
    barin = tk.Frame(bar, bg=COLORS['bg_2'])
    barin.pack(fill='x', padx=22, pady=12)

    _btn(barin, _t(app, 'View Full Changelog'), primary=False,
         command=lambda: _open_changelog(app)).pack(side='left')
    _btn(barin, _t(app, 'Support Development'), primary=False,
         command=lambda: _open(KOFI_URL)).pack(side='left', padx=8)
    _btn(barin, _t(app, 'Start Building'), primary=True,
         command=_dismiss).pack(side='right')

    win.bind('<Escape>', lambda e: _dismiss())
    win.bind('<Return>', lambda e: _dismiss())
    try:
        win.focus_set()
    except Exception:
        pass


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
    card = tk.Frame(parent, bg=COLORS['bg_2'],
                    highlightbackground=COLORS['border_2'],
                    highlightthickness=1)
    inner = tk.Frame(card, bg=COLORS['bg_2'])
    inner.pack(fill='both', expand=True, padx=16, pady=14)

    # Title row
    trow = tk.Frame(inner, bg=COLORS['bg_2'])
    trow.pack(fill='x')
    tk.Label(trow, text='\u2764  Support Development', font=FONTS['h3'],
             bg=COLORS['bg_2'], fg=COLORS['fg_0']).pack(side='left')
    tk.Label(trow, text='v' + RELEASE.get('version', ''),
             font=FONTS['meta'], bg=COLORS['bg_2'],
             fg=COLORS['fg_4']).pack(side='right')

    # Live stat line (filled async)
    stat_var = tk.StringVar(value='Downloads: \u2026   Releases: \u2026')
    tk.Label(inner, textvariable=stat_var, font=FONTS['label'],
             bg=COLORS['bg_2'], fg=COLORS['fg_3'],
             anchor='w').pack(fill='x', pady=(8, 2))

    tk.Label(inner,
             text=('Donations are appreciated but never required \u2014 this '
                   'tool will always be free and open source.'),
             font=FONTS['meta'], bg=COLORS['bg_2'], fg=COLORS['fg_4'],
             anchor='w', justify='left', wraplength=320).pack(
                 fill='x', pady=(0, 12))

    _btn(inner, _t(app, 'Support Project'), primary=True,
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

    win = tk.Toplevel(app)
    win.title(op_label + ' Complete')
    win.configure(bg=COLORS['bg_1'])
    win.transient(app)
    win.resizable(False, False)
    try:
        win.grab_set()
    except Exception:
        pass

    W, H = 420, 250
    try:
        app.update_idletasks()
        x = app.winfo_x() + (app.winfo_width() - W) // 2
        y = app.winfo_y() + (app.winfo_height() - H) // 2
        win.geometry('%dx%d+%d+%d' % (W, H, max(0, x), max(0, y)))
    except Exception:
        win.geometry('%dx%d' % (W, H))

    tk.Frame(win, bg=COLORS['success'], height=4).pack(fill='x')
    body = tk.Frame(win, bg=COLORS['bg_1'])
    body.pack(fill='both', expand=True, padx=24, pady=18)

    tk.Label(body, text='\u2713  ' + op_label + ' Complete',
             font=FONTS['h3'], bg=COLORS['bg_1'],
             fg=COLORS['success_hi'], anchor='w').pack(fill='x')
    tk.Label(body, text=_success_line(op_label), font=FONTS['label'],
             bg=COLORS['bg_1'], fg=COLORS['fg_3'], anchor='w',
             justify='left', wraplength=W - 60).pack(fill='x', pady=(4, 14))

    tk.Frame(body, bg=COLORS['border_2'], height=1).pack(fill='x')
    tk.Label(body, text='\u2764  Enjoying exFAT Image Builder?',
             font=FONTS['body_b'], bg=COLORS['bg_1'],
             fg=COLORS['fg_1'], anchor='w').pack(fill='x', pady=(12, 2))
    tk.Label(body, text='Support helps fund future development.',
             font=FONTS['label'], bg=COLORS['bg_1'], fg=COLORS['fg_3'],
             anchor='w', justify='left',
             wraplength=W - 60).pack(fill='x', pady=(0, 14))

    btns = tk.Frame(body, bg=COLORS['bg_1'])
    btns.pack(fill='x')

    def _close():
        try:
            win.grab_release()
        except Exception:
            pass
        win.destroy()

    _btn(btns, _t(app, 'Close'), primary=False,
         command=_close).pack(side='right')
    _btn(btns, _t(app, 'Support Development'), primary=True,
         command=lambda: (_open(KOFI_URL), _close())).pack(
             side='right', padx=(0, 8))

    win.bind('<Escape>', lambda e: _close())
    win.bind('<Return>', lambda e: _close())
    try:
        win.focus_set()
    except Exception:
        pass


def _success_line(op_label):
    low = (op_label or '').lower()
    if 'build' in low:
        return 'Image created successfully.'
    if 'extract' in low:
        return 'Extraction completed successfully.'
    if 'convert' in low:
        return 'Conversion completed successfully.'
    return 'Operation completed successfully.'
