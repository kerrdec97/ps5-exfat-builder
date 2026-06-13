"""
ui/shared/page_head.py — shared "spacious page" helpers.

Three building blocks lifted from ui/tab_exfat.py (the Build tab, which is
the visual gold standard for this app). Pulled into ui/shared so other
tabs can match the same spacious look without re-deriving the colours,
fonts, and paddings each time.

Public API:
    make_themed_button(parent, text, command, kind='primary',
                       icon=None, font_size=10, padx=14, pady=7,
                       state='normal') -> tk.Button
        Map design-system buttons (primary / success / warn / danger /
        ghost) to colour tokens. The same look as the green "Build All"
        and purple "+ Add to Queue" buttons in the Build tab.

    info_banner(parent, text, on_click=None) -> tk.Frame
        Slim accent-tinted strip with an "i" glyph + descriptive text,
        optional click handler. Matches the "Tip: Recommended Size,
        Cluster..." banner.

    page_head(parent, badge_emoji, title_text, subtitle_text) -> tk.Frame
        Title row with a big accented badge tile, h2 title, and meta
        subtitle. Pack with `pady=(14, 12)` for the standard spacing.
"""
import tkinter as tk

from tkinter_theme import COLORS, FONTS


def make_themed_button(parent, text, command, kind='primary',
                        icon=None, font_size=10, padx=16, pady=8,
                        state='normal'):
    """Build a tk.Button styled like the design-system btn-* classes.

    `kind` values:
        'primary'      — accent (purple) fill, white text. Use for the
                         main call-to-action on a page (Add to Queue,
                         Scan, Apply, etc).
        'success'      — muted teal fill, white text. Use for secondary
                         forward actions like Build All, Convert,
                         Extract, Connect. Reads as "go" without
                         shouting like bright green.
        'success_done' — bright green fill. Use ONLY for explicit
                         "verified/done/connected" states. Almost
                         never the right pick for an action button.
        'warn'         — amber fill, dark text. Pause / caution.
        'danger'       — red fill, white text. Destructive.
        'ghost'        — transparent fill, fg_3 text, border outline.
                         For tertiary actions (Force Dismount).
    """
    schemes = {
        'primary':      (COLORS['accent'],  COLORS['fg_0'],
                         COLORS['accent_hi'], COLORS['fg_0']),
        # 'success' is the secondary action colour — muted teal that
        # sits next to the brand purple without clashing. Replaces the
        # old bright mint that looked like a stoplight.
        'success':      (COLORS['teal'],    COLORS['fg_0'],
                         COLORS['teal_hi'], COLORS['fg_0']),
        # Genuine "done" green — kept for any future status buttons
        # that need the mint colour, but not used for actions.
        'success_done': (COLORS['success'], COLORS['fg_0'],
                         COLORS['success_hi'], COLORS['fg_0']),
        'warn':         (COLORS['warn'],    '#1a0e00',
                         COLORS['warn_hi'], '#1a0e00'),
        'danger':       (COLORS['danger'],  COLORS['fg_0'],
                         COLORS['danger_hi'], COLORS['fg_0']),
        'ghost':        (COLORS['bg_3'],    COLORS['fg_2'],
                         COLORS['bg_4'],    COLORS['fg_1']),
    }
    bg, fg, abg, afg = schemes.get(kind, schemes['primary'])
    label = (icon + '  ' + text) if icon else text
    btn = tk.Button(parent, text=label,
                    font=(FONTS['button'][0], font_size, 'bold'),
                    bg=bg, fg=fg,
                    activebackground=abg, activeforeground=afg,
                    relief='flat', bd=0,
                    padx=padx, pady=pady,
                    cursor='hand2', state=state, takefocus=1,
                    highlightthickness=1,
                    highlightbackground=bg,
                    highlightcolor=COLORS['accent_hi'],
                    command=command)
    if kind == 'ghost':
        btn.configure(highlightbackground=COLORS['border_3'],
                      highlightthickness=1)
    return btn


def info_banner(parent, text, on_click=None):
    """Slim accent-tinted info bar."""
    bg = COLORS['accent_08']
    border = COLORS['accent_lo']
    bar = tk.Frame(parent, bg=bg,
                   highlightbackground=border, highlightthickness=1)
    tk.Frame(bar, bg=COLORS['accent'], width=3).pack(
        side='left', fill='y')
    inner = tk.Frame(bar, bg=bg)
    inner.pack(side='left', fill='x', expand=True, padx=14, pady=11)

    ico = tk.Label(inner, text='i',
                   bg=COLORS['accent'], fg=COLORS['fg_0'],
                   font=(FONTS['body'][0], 9, 'bold'),
                   width=2, padx=2, pady=0)
    ico.pack(side='left', padx=(0, 10))

    msg = tk.Label(inner, text=text,
                   font=FONTS['body'],
                   bg=bg, fg=COLORS['accent_hi'],
                   anchor='w', justify='left', wraplength=900)
    msg.pack(side='left', fill='x', expand=True)

    if on_click:
        for w in [bar, inner, ico, msg]:
            w.bind('<Button-1>', lambda e: on_click(), add='+')
            try:
                w.config(cursor='hand2')
            except Exception:
                pass

    return bar


def page_head(parent, badge_emoji, title_text, subtitle_text):
    """Top-of-page header with a gradient-ish badge tile + title + subtitle.

    Pack with `pady=(14, 12)` for the Build-tab standard spacing.
    """
    bg = COLORS['bg_2']
    head = tk.Frame(parent, bg=bg,
                    highlightbackground=COLORS['border_3'],
                    highlightthickness=1)
    tk.Frame(head, bg=COLORS['accent'], width=4).pack(
        side='left', fill='y')
    inner = tk.Frame(head, bg=bg)
    inner.pack(side='left', fill='x', expand=True, padx=18, pady=16)

    badge = tk.Label(inner, text=badge_emoji,
                     bg=COLORS['accent_08'], fg=COLORS['accent_hi'],
                     font=(FONTS['body'][0], 16, 'bold'),
                     width=3, height=2, padx=4, pady=0,
                     highlightbackground=COLORS['accent_lo'],
                     highlightthickness=1)
    badge.pack(side='left', padx=(0, 14))

    text_col = tk.Frame(inner, bg=bg)
    text_col.pack(side='left', fill='x', expand=True)
    tk.Label(text_col, text=title_text,
             font=(FONTS['h2'][0], 17, 'bold'),
             bg=bg, fg=COLORS['fg_0'], anchor='w'
             ).pack(fill='x')
    tk.Label(text_col, text=subtitle_text,
             font=FONTS['meta'],
             bg=bg, fg=COLORS['fg_3'], anchor='w',
             justify='left', wraplength=900
             ).pack(fill='x', pady=(4, 0))

    return head


def field_block(parent, label_text, var, on_browse=None,
                browse_text='Browse', hint=None):
    """Label-above-input field block with optional Browse button.

    Standard spacious form field used across redesigned tabs:
      - Bold label on its own row, optional grey "hint" continuation
      - Themed input wrap with border_3 outline and field_bg background
      - Optional Browse button on the right side of the input

    Returns the outer block Frame so callers can pack additional
    siblings inside if needed.
    """
    block = tk.Frame(parent, bg=COLORS['bg_2'])
    block.pack(fill='x', pady=(14, 0))

    lbl_row = tk.Frame(block, bg=COLORS['bg_2'])
    lbl_row.pack(fill='x')
    tk.Label(lbl_row, text=label_text,
             font=(FONTS['body'][0], 9, 'bold'),
             bg=COLORS['bg_2'], fg=COLORS['fg_2'], anchor='w'
             ).pack(side='left')
    if hint:
        tk.Label(lbl_row, text='  \u2022  ' + hint,
                 font=FONTS['meta'],
                 bg=COLORS['bg_2'], fg=COLORS['fg_5']
                 ).pack(side='left')

    input_wrap = tk.Frame(block, bg=COLORS['field_bg'],
                          highlightbackground=COLORS['border_3'],
                          highlightthickness=1)
    input_wrap.pack(fill='x', pady=(6, 0))

    entry = tk.Entry(input_wrap, textvariable=var,
                     font=FONTS['mono_sm'],
                     bg=COLORS['field_bg'], fg=COLORS['field_fg'],
                     insertbackground=COLORS['field_fg'],
                     selectbackground=COLORS['accent'],
                     selectforeground=COLORS['fg_0'],
                     relief='flat', bd=9)
    entry.pack(side='left', fill='x', expand=True)

    if on_browse:
        tk.Button(input_wrap, text=browse_text,
                  font=FONTS['button'],
                  bg=COLORS['bg_3'], fg=COLORS['fg_2'],
                  activebackground=COLORS['bg_5'],
                  activeforeground=COLORS['accent'],
                  relief='flat', bd=0,
                  padx=16, pady=8,
                  cursor='hand2',
                  command=on_browse
                  ).pack(side='right')

    return block
