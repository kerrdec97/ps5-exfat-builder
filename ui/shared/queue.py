"""
ui/shared/queue.py — build-queue and progress display components.

Components
----------
- BuildQueueItem — one row in a queue list: optional cover thumb,
                   title + meta line, a StatusBadge, and an optional
                   right-side action widget slot.
- QueuePanel     — a scroll-free vertical stack of BuildQueueItems with
                   a header (title + count badge) and an action slot.
                   `set_items()` rebuilds the list from plain dicts.
- ProgressPanel  — a labelled progress display: caption, a determinate
                   bar, and a sub-line for byte/ETA text. `set_progress`
                   updates it.

IMPORTANT: these components are pure presentation. QueuePanel renders
whatever list of dicts it is handed — it does NOT read self._queue or
call _run_queue / _clear_queue. A later stage will feed it data from the
real `self._queue` and wire header buttons to the real methods.

Stage 2 (UI refactor): standalone, not wired into the app yet.
"""

import tkinter as tk
from tkinter import ttk

from tkinter_theme import (
    COLORS, FONTS, SPACING,
    get_color, get_font,
)
from ui.shared.badges import StatusBadge


class BuildQueueItem(tk.Frame):
    """A single queue row.

    Args:
        parent : container
        title  : game / job name
        meta   : secondary line (e.g. "PPSA12345 · v1.00 · 45 GB")
        status : a STATUS_COLORS key for the badge (default "queued")

    `self.actions` is a right-aligned frame for the host to add a real
    control (e.g. a remove button wired to the actual queue). The row
    invents no behavior.
    """

    def __init__(self, parent, title="", meta="", status="queued",
                 **kwargs):
        self._bg = get_color("bg_3")
        super().__init__(parent, bg=self._bg,
                         highlightbackground=get_color("border_3"),
                         highlightcolor=get_color("border_3"),
                         highlightthickness=1, bd=0, **kwargs)

        pad = SPACING["md"]

        text_col = tk.Frame(self, bg=self._bg)
        text_col.pack(side="left", fill="x", expand=True,
                      padx=pad, pady=SPACING["sm"])
        tk.Label(text_col, text=title, bg=self._bg,
                 fg=get_color("fg_0"), font=get_font("body_b"),
                 anchor="w").pack(anchor="w")
        if meta:
            tk.Label(text_col, text=meta, bg=self._bg,
                     fg=get_color("fg_4"), font=get_font("meta"),
                     anchor="w").pack(anchor="w",
                                      pady=(SPACING["xxs"], 0))

        right = tk.Frame(self, bg=self._bg)
        right.pack(side="right", padx=pad)
        self.badge = StatusBadge(right, status)
        self.badge.pack(side="left", padx=(0, SPACING["sm"]))
        self.actions = tk.Frame(right, bg=self._bg)
        self.actions.pack(side="left")

    def set_status(self, status, text=None):
        self.badge.set_status(status, text)


class QueuePanel(tk.Frame):
    """A header + vertical list of BuildQueueItems.

    Usage (later stage — fed from the real queue):
        qp = QueuePanel(parent, title="Build Queue")
        qp.pack(fill="both", expand=True)
        # host adds real buttons to qp.actions:
        ttk.Button(qp.actions, text="Clear",
                   command=app._clear_queue).pack(side="right")
        # host feeds rows derived from app._queue:
        qp.set_items([
            {"title": g.name, "meta": g.meta, "status": g.status}
            for g in app._queue
        ])

    set_items() takes a list of plain dicts with keys: title, meta,
    status. It does not touch any app state.
    """

    def __init__(self, parent, title="Queue", **kwargs):
        self._bg = get_color("bg_2")
        super().__init__(parent, bg=self._bg,
                         highlightbackground=get_color("border_3"),
                         highlightcolor=get_color("border_3"),
                         highlightthickness=1, bd=0, **kwargs)
        pad = SPACING["lg"]

        header = tk.Frame(self, bg=self._bg)
        header.pack(fill="x", padx=pad, pady=(pad, SPACING["sm"]))
        tk.Label(header, text=title, bg=self._bg,
                 fg=get_color("fg_0"), font=get_font("h3")).pack(side="left")
        self._count = StatusBadge(header, "idle", text="0 items")
        self._count.pack(side="left", padx=(SPACING["sm"], 0))
        self.actions = tk.Frame(header, bg=self._bg)
        self.actions.pack(side="right")

        self._list = tk.Frame(self, bg=self._bg)
        self._list.pack(fill="both", expand=True, padx=pad,
                        pady=(0, pad))

        self._empty = tk.Label(self._list, text="Queue is empty.",
                               bg=self._bg, fg=get_color("fg_5"),
                               font=get_font("body"))
        self._rows = []
        self._render_empty()

    def _render_empty(self):
        self._empty.pack(pady=SPACING["lg"])

    def set_items(self, items):
        """Rebuild the row list from a list of dicts.

        Each dict: {"title": str, "meta": str, "status": str}.
        Passing [] shows the empty state.
        """
        for r in self._rows:
            r.destroy()
        self._rows = []
        self._empty.pack_forget()

        items = items or []
        for it in items:
            row = BuildQueueItem(
                self._list,
                title=it.get("title", ""),
                meta=it.get("meta", ""),
                status=it.get("status", "queued"),
            )
            row.pack(fill="x", pady=SPACING["xs"])
            self._rows.append(row)

        n = len(items)
        self._count.set_status("running" if n else "idle",
                               "%d item%s" % (n, "" if n == 1 else "s"))
        if not items:
            self._render_empty()


class ProgressPanel(tk.Frame):
    """A labelled determinate-progress display.

    Usage:
        pp = ProgressPanel(parent, caption="Copying files…")
        pp.pack(fill="x")
        ...
        pp.set_progress(72.4, detail="512 MB / 707 MB · ETA 00:08")

    Pure display — it has no timer and starts no work.
    """

    def __init__(self, parent, caption="", bg="bg_2", **kwargs):
        self._bg = get_color(bg)
        super().__init__(parent, bg=self._bg,
                         highlightthickness=0, bd=0, **kwargs)

        self._caption = tk.Label(self, text=caption, bg=self._bg,
                                 fg=get_color("fg_1"),
                                 font=get_font("body"), anchor="w")
        self._caption.pack(fill="x")

        self._bar = ttk.Progressbar(
            self, style="Build.Horizontal.TProgressbar",
            mode="determinate", value=0)
        self._bar.pack(fill="x", pady=(SPACING["sm"], SPACING["xs"]))

        self._detail = tk.Label(self, text="", bg=self._bg,
                                fg=get_color("fg_4"),
                                font=get_font("meta"), anchor="w")
        self._detail.pack(fill="x")

    def set_progress(self, percent, detail=None, caption=None):
        """Update bar value (0-100), and optionally detail / caption."""
        try:
            self._bar.configure(value=max(0, min(100, float(percent))))
        except Exception:
            pass
        if detail is not None:
            self._detail.configure(text=detail)
        if caption is not None:
            self._caption.configure(text=caption)


# ── harmless self-test / demo ────────────────────────────────────────
if __name__ == "__main__":
    from tkinter_theme import apply_theme
    root = tk.Tk()
    root.title("queue.py — preview")
    apply_theme(root)
    root.geometry("520x460")

    qp = QueuePanel(root, title="Build Queue")
    qp.pack(fill="x", padx=16, pady=16)
    qp.set_items([
        {"title": "Silent Hill 2 Remake",
         "meta": "PPSA34038 · v1.100 · 45.2 GB", "status": "queued"},
        {"title": "Stellar Blade",
         "meta": "PPSA12345 · v1.010 · 34.8 GB", "status": "running"},
        {"title": "God of War Ragnarok",
         "meta": "PPSA16789 · v1.200 · 88.7 GB", "status": "queued"},
    ])

    pp = ProgressPanel(root, caption="Copying files…", bg="bg_1")
    pp.pack(fill="x", padx=16, pady=(0, 16))
    pp.set_progress(72.4, detail="512 MB / 707 MB · 18.3 MB/s · ETA 00:08")

    root.mainloop()
