"""Home control-center dashboard.

Presentation-only landing surface. Every action routes through the existing
tab registry and introduces no workflow or backend behavior.
"""

import tkinter as tk

from tkinter_theme import COLORS, FONTS, SPACING

from exfat_builder import _
from ui.shared.page_head import make_themed_button
from ui.shared.scroll import attach_scroll


def build_home_tab(parent, app):
    """Build the Home dashboard into ``parent``."""
    parent.configure(bg=COLORS['bg_1'])

    page_canvas = tk.Canvas(parent, bg=COLORS['bg_1'], highlightthickness=0)
    page_sb = tk.Scrollbar(
        parent, orient='vertical', command=page_canvas.yview,
        bg=COLORS['bg_3'], troughcolor=COLORS['bg_1'])
    page_canvas.configure(yscrollcommand=page_sb.set)
    page_sb.pack(side='right', fill='y')
    page_canvas.pack(side='left', fill='both', expand=True)

    body = tk.Frame(page_canvas, bg=COLORS['bg_1'])
    window_id = page_canvas.create_window((0, 0), window=body, anchor='nw')
    body.bind(
        '<Configure>',
        lambda _e: page_canvas.configure(
            scrollregion=page_canvas.bbox('all')))
    page_canvas.bind(
        '<Configure>',
        lambda event: page_canvas.itemconfig(window_id, width=event.width))
    attach_scroll(page_canvas)

    def _go(key):
        try:
            getattr(app, '_switch_tab', lambda _key: None)(key)
        except Exception:
            pass

    content = tk.Frame(body, bg=COLORS['bg_1'])
    content.pack(fill='x', padx=SPACING['xxl'], pady=(SPACING['xl'], 28))

    hero = tk.Frame(
        content, bg=COLORS['bg_2'],
        highlightbackground=COLORS['border_3'], highlightthickness=1)
    hero.pack(fill='x')
    tk.Frame(hero, bg=COLORS['accent'], width=4).pack(side='left', fill='y')

    hero_inner = tk.Frame(hero, bg=COLORS['bg_2'])
    hero_inner.pack(fill='both', expand=True, padx=24, pady=22)

    hero_copy = tk.Frame(hero_inner, bg=COLORS['bg_2'])
    hero_copy.pack(fill='x')
    tk.Label(
        hero_copy, text=_('CONTROL CENTER'),
        font=FONTS['eyebrow'], bg=COLORS['bg_2'], fg=COLORS['accent'],
        anchor='w').pack(fill='x')
    tk.Label(
        hero_copy, text=_('What would you like to do?'),
        font=(FONTS['h1'][0], 24, 'bold'),
        bg=COLORS['bg_2'], fg=COLORS['fg_0'], anchor='w'
    ).pack(fill='x', pady=(6, 0))
    tk.Label(
        hero_copy,
        text=_(
            'Build, inspect, organize, and deploy game images from one '
            'workspace.'),
        font=FONTS['body'], bg=COLORS['bg_2'], fg=COLORS['fg_3'],
        anchor='w', justify='left', wraplength=560
    ).pack(fill='x', pady=(6, 0))

    hero_actions = tk.Frame(hero_copy, bg=COLORS['bg_2'])
    hero_actions.pack(fill='x', pady=(18, 0))
    make_themed_button(
        hero_actions, _('Build Image'), command=lambda: _go('unibuild'),
        kind='primary', icon='+', padx=20, pady=9
    ).pack(side='left')
    make_themed_button(
        hero_actions, _('Open Library'), command=lambda: _go('library'),
        kind='ghost', padx=18, pady=9
    ).pack(side='left', padx=(8, 0))

    capability = tk.Frame(
        hero_inner, bg=COLORS['bg_3'],
        highlightbackground=COLORS['border_2'], highlightthickness=1)
    capability.pack(fill='x', pady=(18, 0))
    capabilities = (
        ('3', _('IMAGE FORMATS'), COLORS['accent_hi']),
        ('1', _('BATCH QUEUE'), COLORS['teal_hi']),
        ('PS5', _('TOOLKIT'), COLORS['purple_hi']),
    )
    for index, (value, caption, color) in enumerate(capabilities):
        cell = tk.Frame(capability, bg=COLORS['bg_3'])
        cell.pack(side='left', fill='both', expand=True,
                  padx=(16 if index == 0 else 8, 16), pady=10)
        tk.Label(
            cell, text=value, font=(FONTS['mono'][0], 12, 'bold'),
            bg=COLORS['bg_3'], fg=color, width=5, anchor='w'
        ).pack(side='left')
        tk.Label(
            cell, text=caption, font=FONTS['eyebrow'],
            bg=COLORS['bg_3'], fg=COLORS['fg_4'], anchor='w'
        ).pack(side='left')

    def _section_header(title, subtitle):
        row = tk.Frame(content, bg=COLORS['bg_1'])
        row.pack(fill='x', pady=(24, 10))
        tk.Label(
            row, text=title, font=FONTS['h2'],
            bg=COLORS['bg_1'], fg=COLORS['fg_0'], anchor='w'
        ).pack(anchor='w')
        tk.Label(
            row, text=subtitle, font=FONTS['meta'],
            bg=COLORS['bg_1'], fg=COLORS['fg_4'], anchor='w'
        ).pack(anchor='w', pady=(2, 0))

    def _action_card(parent_widget, key, icon, eyebrow, title, desc,
                     accent='accent', primary=False):
        card_bg = COLORS['bg_2'] if primary else COLORS['bg_3']
        border = COLORS[accent] if primary else COLORS['border_3']
        card = tk.Frame(
            parent_widget, bg=card_bg,
            highlightbackground=border, highlightthickness=1)

        inner = tk.Frame(card, bg=card_bg)
        inner.pack(fill='both', expand=True, padx=20, pady=18)

        top = tk.Frame(inner, bg=card_bg)
        top.pack(fill='x')
        icon_tile = tk.Frame(
            top, bg=COLORS['accent_08'], width=44, height=44,
            highlightbackground=COLORS['accent_15'], highlightthickness=1)
        icon_tile.pack(side='left')
        icon_tile.pack_propagate(False)
        tk.Label(
            icon_tile, text=icon, font=(FONTS['body'][0], 18, 'bold'),
            bg=COLORS['accent_08'], fg=COLORS[accent]
        ).pack(expand=True)

        title_col = tk.Frame(top, bg=card_bg)
        title_col.pack(side='left', fill='x', expand=True, padx=(12, 0))
        tk.Label(
            title_col, text=eyebrow.upper(), font=FONTS['eyebrow'],
            bg=card_bg, fg=COLORS[accent], anchor='w'
        ).pack(fill='x')
        tk.Label(
            title_col, text=title,
            font=(FONTS['h3'][0], 13, 'bold'),
            bg=card_bg, fg=COLORS['fg_0'], anchor='w'
        ).pack(fill='x', pady=(2, 0))

        description = tk.Label(
            inner, text=desc, font=FONTS['body'],
            bg=card_bg, fg=COLORS['fg_3'], anchor='w',
            justify='left', wraplength=220)
        description.pack(fill='x', pady=(14, 18))
        card.bind(
            '<Configure>',
            lambda event, label=description: label.configure(
                wraplength=max(150, event.width - 44)),
            add='+')

        make_themed_button(
            inner, _('Open'), command=lambda route=key: _go(route),
            kind='primary' if primary else 'ghost',
            font_size=9, padx=16, pady=7
        ).pack(anchor='w')
        return card

    _section_header(
        _('Core workflows'),
        _('Start with a game folder or work with an existing image.'))

    workflow_grid = tk.Frame(content, bg=COLORS['bg_1'])
    workflow_grid.pack(fill='x')
    workflow_grid.grid_columnconfigure(0, weight=3, uniform='workflow')
    workflow_grid.grid_columnconfigure(1, weight=2, uniform='workflow')

    build_card = _action_card(
        workflow_grid, 'unibuild', '+', _('PRIMARY WORKFLOW'),
        _('Build Image'), _('Create a new image from a game folder.'),
        accent='accent', primary=True)
    build_card.grid(
        row=0, column=0, rowspan=2, sticky='nsew', padx=(0, 12))

    extract_card = _action_card(
        workflow_grid, 'extract', '\u2191', _('EXISTING IMAGE'),
        _('Extract Image'), _('Unpack an image back into files and folders.'),
        accent='teal')
    extract_card.grid(row=0, column=1, sticky='nsew', pady=(0, 12))

    convert_card = _action_card(
        workflow_grid, 'convert', '\u21c4', _('FORMAT TOOLS'),
        _('Convert Image'),
        _('Change an existing image between supported formats.'),
        accent='purple')
    convert_card.grid(row=1, column=1, sticky='nsew')

    _section_header(
        _('Workspace'),
        _('Manage local content, console tools, and support resources.'))

    workspace_grid = tk.Frame(content, bg=COLORS['bg_1'])
    workspace_grid.pack(fill='x')
    for column in range(3):
        workspace_grid.grid_columnconfigure(
            column, weight=1, uniform='workspace')

    workspace_cards = (
        ('library', '\u25a6', _('COLLECTION'), _('Library'),
         _('Browse and manage your local game images.'), 'teal'),
        ('ps5', '\u25ce', _('CONSOLE'), _('PS5 Tools'),
         _('Connect, transfer, inspect logs, and manage tools.'), 'accent'),
        ('report', '?', _('SUPPORT'), _('Report Issue'),
         _('Create a diagnostic report or get help.'), 'purple'),
    )
    workspace_widgets = []
    for column, card_data in enumerate(workspace_cards):
        card = _action_card(workspace_grid, *card_data)
        workspace_widgets.append(card)
        card.grid(
            row=0, column=column, sticky='nsew',
            padx=(0 if column == 0 else 12, 0))

    layout_state = {'compact': None}

    def _responsive_layout(event):
        compact = event.width < 780
        if layout_state['compact'] == compact:
            return
        layout_state['compact'] = compact

        for card in (build_card, extract_card, convert_card):
            card.grid_forget()
        if compact:
            workflow_grid.grid_columnconfigure(0, weight=1, uniform='')
            workflow_grid.grid_columnconfigure(1, weight=0, uniform='')
            build_card.grid(row=0, column=0, sticky='nsew')
            extract_card.grid(
                row=1, column=0, sticky='nsew', pady=(12, 0))
            convert_card.grid(
                row=2, column=0, sticky='nsew', pady=(12, 0))
        else:
            workflow_grid.grid_columnconfigure(
                0, weight=3, uniform='workflow')
            workflow_grid.grid_columnconfigure(
                1, weight=2, uniform='workflow')
            build_card.grid(
                row=0, column=0, rowspan=2, sticky='nsew',
                padx=(0, 12))
            extract_card.grid(
                row=0, column=1, sticky='nsew', pady=(0, 12))
            convert_card.grid(row=1, column=1, sticky='nsew')

        for card in workspace_widgets:
            card.grid_forget()
        if compact:
            workspace_grid.grid_columnconfigure(0, weight=1, uniform='')
            workspace_grid.grid_columnconfigure(1, weight=0, uniform='')
            workspace_grid.grid_columnconfigure(2, weight=0, uniform='')
            for row, card in enumerate(workspace_widgets):
                card.grid(
                    row=row, column=0, sticky='nsew',
                    pady=(0 if row == 0 else 12, 0))
        else:
            for column in range(3):
                workspace_grid.grid_columnconfigure(
                    column, weight=1, uniform='workspace')
            for column, card in enumerate(workspace_widgets):
                card.grid(
                    row=0, column=column, sticky='nsew',
                    padx=(0 if column == 0 else 12, 0))

    page_canvas.bind('<Configure>', _responsive_layout, add='+')
