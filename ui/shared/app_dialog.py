"""
ui/shared/app_dialog.py — v4.0 Design System: AppDialog (§4.9)
==============================================================

The one themed modal that replaces every native tkinter `messagebox`. Native
message boxes are light-grey Windows dialogs that crack the dark theme — the
single biggest "unfinished" signal in the app (UI/UX audit, component §4.9).

This module is ADDITIVE and self-contained:
  * imports only tkinter + the design-system semantic layer (theme_tokens);
    it does NOT import exfat_builder, so it adds no double-import surface and
    cannot change any existing screen just by existing;
  * nothing calls it until call sites are migrated (that happens in later,
    individually testable batches — see V4.0.0_RELEASE_PLAN.md).

MIGRATION CONTRACT (why the shims exist)
----------------------------------------
The `app_*` helpers mirror messagebox's names and RETURN VALUES so migrating a
call site is a minimal diff:

    messagebox.showinfo(title, msg)            -> app_info(parent, title, msg)
    messagebox.showwarning(title, msg)         -> app_warning(parent, title, msg)
    messagebox.showerror(title, msg)           -> app_error(parent, title, msg)
    messagebox.askyesno(title, msg)  -> bool   -> app_confirm(parent, title, msg)  -> bool
    (destructive confirm)            -> bool   -> app_danger(parent, title, msg, ok_label='Delete') -> bool

`parent` is any widget on the target window (usually the tab frame or `app`).
Like messagebox, these are MODAL and BLOCK, so they must be called on the Tk
main thread (worker threads already marshal via `after(0, ...)` — unchanged).

Extra affordances the native boxes can't do (optional, all keyword args):
  * detail=  a monospace sub-box for a path / id (e.g. a backup .zip path)
  * note=    an amber caution line under the message
These let the Apply-Backport confirmation become formatted sections instead of
a wall of text, once migrated.
"""

import tkinter as tk

# theme_tokens lives in ui/ but the app runs with the repo root on sys.path
# (and the PyInstaller bundle flattens it), so try both import spellings.
try:
    from ui import theme_tokens as T
except Exception:
    import theme_tokens as T


# Variant -> (glyph, accent colour)
_VARIANTS = {
    "info": {"glyph": "ℹ", "accent": T.INFO},
    "warning": {"glyph": "⚠", "accent": T.WARNING},
    "error": {"glyph": "✖", "accent": T.DANGER},
    "success": {"glyph": "✓", "accent": T.GO},
    "confirm": {"glyph": "❓", "accent": T.PRIMARY},
    "danger": {"glyph": "⚠", "accent": T.DANGER},
}


def _themed_button(parent, text, variant, command):
    """A single design-system button (theme_tokens.button bundle) with hover.
    Kept local so the dialog depends only on tkinter + theme_tokens."""
    b = T.button(variant)
    bg = b["bg"] if b["bg"] else T.BG_CARD
    btn = tk.Button(
        parent, text=text, command=command,
        font=T.FONT_BODY, fg=b["fg"], bg=bg,
        activeforeground=b["fg"], activebackground=b["active"] or T.BG_RAISED,
        relief="flat", bd=0, cursor="hand2",
        highlightthickness=1 if b["border"] else 0,
        highlightbackground=b["border"] or bg,
        highlightcolor=b["border"] or bg,
        padx=T.SPACE["lg"], pady=T.SPACE["sm"])
    # hover: swap to the variant's hover colour and back
    _hover_bg = b["hover"] or T.BG_RAISED
    _rest_bg = bg
    btn.bind("<Enter>", lambda _e: btn.configure(bg=_hover_bg))
    btn.bind("<Leave>", lambda _e: btn.configure(bg=_rest_bg))
    return btn


class AppDialog(tk.Toplevel):
    """Themed modal dialog. Prefer the module-level helpers (info/confirm/…)
    over constructing this directly.

    Result contract: `.result` is True/False for confirm-style dialogs (True =
    primary/confirm pressed) and None for acknowledge-only dialogs. When
    `alt_label` is given the dialog is TRI-STATE (askyesnocancel replacement):
    primary → True, alt → False, cancel/Escape/close → None. Escape and the
    window-close button always resolve to the cancel/None outcome.
    """

    def __init__(self, parent, title, message, *,
                 variant="info", detail=None, note=None,
                 ok_label="OK", cancel_label=None, alt_label=None,
                 ok_variant=None):
        super().__init__(parent)
        self.result = None
        self._alt = alt_label is not None
        # What Escape / close resolve to: None when tri-state, False for a
        # two-button confirm, None for acknowledge-only.
        self._cancel_value = None if self._alt else (
            False if cancel_label is not None
            else None)

        meta = _VARIANTS.get(variant, _VARIANTS["info"])
        if ok_variant is None:
            ok_variant = {"danger": "danger_filled",
                          "confirm": "primary"}.get(variant, "primary")

        self.withdraw()
        self.title(title or "exFAT Image Builder")
        self.configure(bg=T.BG_APP)
        self.resizable(False, False)
        try:
            self.transient(parent.winfo_toplevel())
        except Exception:
            pass

        # ---- card ----
        card = tk.Frame(self, bg=T.BG_CARD,
                        highlightthickness=1,
                        highlightbackground=T.BORDER_SUBTLE,
                        highlightcolor=T.BORDER_SUBTLE)
        card.pack(fill="both", expand=True,
                  padx=T.SPACE["sm"], pady=T.SPACE["sm"])
        inner = tk.Frame(card, bg=T.BG_CARD)
        inner.pack(fill="both", expand=True,
                   padx=T.SPACE["lg"], pady=T.SPACE["lg"])

        # ---- header: glyph + title ----
        head = tk.Frame(inner, bg=T.BG_CARD)
        head.pack(fill="x")
        tk.Label(head, text=meta["glyph"], font=T.FONT_TITLE,
                 fg=meta["accent"], bg=T.BG_CARD).pack(side="left",
                                                       padx=(0, T.SPACE["sm"]))
        tk.Label(head, text=title or "", font=T.FONT_TITLE,
                 fg=T.TEXT_PRIMARY, bg=T.BG_CARD,
                 anchor="w", justify="left").pack(side="left", fill="x",
                                                  expand=True)

        # ---- message ----
        if message:
            tk.Label(inner, text=message, font=T.FONT_BODY,
                     fg=T.TEXT_PRIMARY, bg=T.BG_CARD,
                     wraplength=420, justify="left", anchor="w").pack(
                fill="x", pady=(T.SPACE["md"], 0))

        # ---- detail (monospace sub-box) ----
        if detail:
            box = tk.Frame(inner, bg=T.BG_SUNKEN,
                           highlightthickness=1,
                           highlightbackground=T.BORDER_SUBTLE)
            box.pack(fill="x", pady=(T.SPACE["md"], 0))
            tk.Label(box, text=detail, font=T.FONT_MONO_SM,
                     fg=T.TEXT_SECONDARY, bg=T.BG_SUNKEN,
                     wraplength=420, justify="left", anchor="w").pack(
                fill="x", padx=T.SPACE["sm"], pady=T.SPACE["sm"])

        # ---- note (amber caution line) ----
        if note:
            tk.Label(inner, text=note, font=T.FONT_SMALL,
                     fg=T.WARNING, bg=T.BG_CARD,
                     wraplength=420, justify="left", anchor="w").pack(
                fill="x", pady=(T.SPACE["md"], 0))

        # ---- buttons (right-aligned, primary rightmost) ----
        bar = tk.Frame(inner, bg=T.BG_CARD)
        bar.pack(fill="x", pady=(T.SPACE["lg"], 0))
        ok_btn = _themed_button(bar, ok_label, ok_variant, self._on_ok)
        ok_btn.pack(side="right")
        if alt_label is not None:
            _themed_button(bar, alt_label, "ghost", self._on_alt).pack(
                side="right", padx=(0, T.SPACE["sm"]))
        if cancel_label is not None:
            _themed_button(bar, cancel_label, "ghost", self._on_cancel).pack(
                side="right", padx=(0, T.SPACE["sm"]))

        # ---- keyboard / close ----
        self.bind("<Return>", lambda _e: self._on_ok())
        self.bind("<Escape>", lambda _e: self._on_cancel())
        self.protocol("WM_DELETE_WINDOW", self._on_cancel)

        self._center_on(parent)
        self.deiconify()
        ok_btn.focus_set()
        try:
            self.grab_set()
        except Exception:
            pass
        self.wait_window(self)

    # ---- outcomes ----
    def _on_ok(self):
        self.result = True
        self._close()

    def _on_alt(self):
        self.result = False
        self._close()

    def _on_cancel(self):
        self.result = self._cancel_value
        self._close()

    def _close(self):
        try:
            self.grab_release()
        except Exception:
            pass
        self.destroy()

    def _center_on(self, parent):
        self.update_idletasks()
        w, h = self.winfo_reqwidth(), self.winfo_reqheight()
        try:
            top = parent.winfo_toplevel()
            px, py = top.winfo_rootx(), top.winfo_rooty()
            pw, ph = top.winfo_width(), top.winfo_height()
            x = px + max(0, (pw - w) // 2)
            y = py + max(0, (ph - h) // 3)
        except Exception:
            x = y = 200
        self.geometry("+%d+%d" % (x, y))


# ---- messagebox-shaped helpers (see MIGRATION CONTRACT above) ----

def app_info(parent, title, message, *, detail=None):
    AppDialog(parent, title, message, variant="info", detail=detail,
              ok_label="OK")


def app_warning(parent, title, message, *, detail=None, note=None):
    AppDialog(parent, title, message, variant="warning", detail=detail,
              note=note, ok_label="OK")


def app_error(parent, title, message, *, detail=None):
    AppDialog(parent, title, message, variant="error", detail=detail,
              ok_label="OK")


def app_success(parent, title, message, *, detail=None):
    AppDialog(parent, title, message, variant="success", detail=detail,
              ok_label="OK")


def app_confirm(parent, title, message, *, detail=None, note=None,
                ok_label="OK", cancel_label="Cancel"):
    """askyesno replacement — returns True if the primary button was pressed."""
    dlg = AppDialog(parent, title, message, variant="confirm", detail=detail,
                    note=note, ok_label=ok_label, cancel_label=cancel_label)
    return bool(dlg.result)


def app_danger(parent, title, message, *, detail=None, note=None,
               ok_label="Delete", cancel_label="Cancel"):
    """Destructive confirm (filled-danger button) — returns True to proceed."""
    dlg = AppDialog(parent, title, message, variant="danger", detail=detail,
                    note=note, ok_label=ok_label, cancel_label=cancel_label,
                    ok_variant="danger_filled")
    return bool(dlg.result)


def app_choose3(parent, title, message, *, detail=None, note=None,
                ok_label="Yes", alt_label="No", cancel_label="Cancel",
                variant="confirm"):
    """askyesnocancel replacement — returns True (primary), False (alt) or
    None (cancel / Escape / window close), matching messagebox semantics.
    Label the buttons as ACTIONS (e.g. 'Overwrite' / 'Save copy') rather than
    Yes/No where possible — that's the whole point of owning the dialog."""
    dlg = AppDialog(parent, title, message, variant=variant, detail=detail,
                    note=note, ok_label=ok_label, alt_label=alt_label,
                    cancel_label=cancel_label)
    return dlg.result


def app_question(parent, title, message, *, detail=None, note=None,
                 yes_label="Yes", no_label="No", variant="confirm"):
    """askquestion replacement — returns the STRING 'yes' or 'no' (matching
    messagebox.askquestion) so callers that compare `== 'yes'` are unchanged.
    Escape / window-close resolve to 'no' (askquestion has no Cancel)."""
    dlg = AppDialog(parent, title, message, variant=variant, detail=detail,
                    note=note, ok_label=yes_label, cancel_label=no_label)
    return "yes" if dlg.result else "no"


__all__ = [
    "AppDialog", "app_info", "app_warning", "app_error", "app_success",
    "app_confirm", "app_danger", "app_choose3", "app_question",
]
