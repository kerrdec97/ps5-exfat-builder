"""
ui/shared/app_dialog.py — the application's themed modal dialog.

Design system › Dialog: a 480px modal on bg_2 with a status icon and a
`title` heading, short body text in fg_2, an optional monospace detail well
and caution line, and a bg_1 footer band with right-aligned buttons (primary
or danger rightmost, minimum width ~96px). Enter = primary, Esc = cancel.

The `app_*` helpers mirror tkinter.messagebox names and RETURN VALUES:

    app_info / app_warning / app_error / app_success (parent, title, msg)
    app_confirm(...)  -> bool          (askyesno)
    app_danger(...)   -> bool          (destructive confirm)
    app_choose3(...)  -> True/False/None  (askyesnocancel)
    app_question(...) -> 'yes' | 'no'  (askquestion)

Like messagebox they are modal and block, so call them on the Tk thread.
Optional keyword args: detail= (monospace well for a path / id),
note= (caution line).
"""

import tkinter as tk

from tkinter_theme import COLORS, FONTS, px
from ui.shared.widgets import Button, IconLabel, Divider, autowrap

C = COLORS

# variant -> (icon, icon colour)
_VARIANTS = {
    "info":    ("info", "accent_hi"),
    "warning": ("warning", "warn"),
    "error":   ("error", "danger"),
    "success": ("success", "success"),
    "confirm": ("help", "accent_hi"),
    "danger":  ("warning", "warn"),
}


def _themed_button(parent, text, variant, command):
    """Compatibility shim: a design-system button."""
    v = {"primary": "primary", "danger_filled": "danger", "danger": "danger",
         "go": "primary"}.get(variant, "secondary")
    return Button(parent, text=text, variant=v, command=command)


class AppDialog(tk.Toplevel):
    """Themed modal dialog. Prefer the module-level helpers.

    `.result` is True/False for confirm-style dialogs (True = primary) and
    None for acknowledge-only. With `alt_label` the dialog is tri-state:
    primary -> True, alt -> False, cancel/Escape/close -> None.
    """

    def __init__(self, parent, title, message, *,
                 variant="info", detail=None, note=None,
                 ok_label="OK", cancel_label=None, alt_label=None,
                 ok_variant=None):
        super().__init__(parent)
        self.result = None
        self._alt = alt_label is not None
        self._cancel_value = None if self._alt else (
            False if cancel_label is not None else None)
        icon, icol = _VARIANTS.get(variant, _VARIANTS["info"])
        if ok_variant is None:
            ok_variant = {"danger": "danger"}.get(variant, "primary")
        ok_variant = {"danger_filled": "danger"}.get(ok_variant, ok_variant)
        if ok_variant not in ("primary", "danger", "secondary"):
            ok_variant = "primary"

        self.withdraw()
        self.title(title or "exFAT Image Builder")
        self.configure(bg=C["border_3"])
        self.resizable(False, False)
        try:
            self.transient(parent.winfo_toplevel())
        except Exception:
            pass

        wrap_w = px(440)
        frame = tk.Frame(self, bg=C["bg_2"], width=px(480))
        frame.pack(fill="both", expand=True)

        body = tk.Frame(frame, bg=C["bg_2"])
        body.pack(fill="both", expand=True, padx=px(24), pady=(px(22), px(20)))
        head = tk.Frame(body, bg=C["bg_2"])
        head.pack(fill="x")
        IconLabel(head, icon, 20, C[icol], C["bg_2"]).pack(
            side="left", anchor="n", padx=(0, px(10)), pady=(px(3), 0))
        tk.Label(head, text=title or "", font=FONTS["title"], fg=C["fg_0"],
                 bg=C["bg_2"], anchor="w", justify="left",
                 wraplength=wrap_w - px(30)).pack(side="left", fill="x",
                                                  expand=True)
        if message:
            tk.Label(body, text=message, font=FONTS["body"], fg=C["fg_2"],
                     bg=C["bg_2"], wraplength=wrap_w, justify="left",
                     anchor="w").pack(fill="x", pady=(px(12), 0))
        if detail:
            box = tk.Frame(body, bg=C["bg_0"], highlightthickness=1,
                           highlightbackground=C["border_2"])
            box.pack(fill="x", pady=(px(12), 0))
            tk.Label(box, text=detail, font=FONTS["code"], fg=C["fg_2"],
                     bg=C["bg_0"], wraplength=wrap_w - px(24),
                     justify="left", anchor="w").pack(
                fill="x", padx=px(12), pady=px(10))
        if note:
            nrow = tk.Frame(body, bg=C["bg_2"])
            nrow.pack(fill="x", pady=(px(12), 0))
            IconLabel(nrow, "warning", 14, C["warn"], C["bg_2"]).pack(
                side="left", anchor="n", padx=(0, px(8)), pady=(px(2), 0))
            tk.Label(nrow, text=note, font=FONTS["caption"], fg=C["warn_hi"],
                     bg=C["bg_2"], wraplength=wrap_w - px(24),
                     justify="left", anchor="w").pack(side="left", fill="x")

        foot = tk.Frame(frame, bg=C["bg_1"])
        foot.pack(fill="x", side="bottom")
        tk.Frame(frame, bg=C["border_2"], height=1).pack(fill="x", side="bottom")
        bar = tk.Frame(foot, bg=C["bg_1"])
        bar.pack(fill="x", padx=px(18), pady=px(10))

        def mk(text, variant, cmd):
            b = Button(bar, text=text, variant=variant, command=cmd,
                       surface=C["bg_1"])
            try:
                import tkinter.font as tkfont
                chars = max(len(text), 8)
                b.configure(width=chars)
            except Exception:
                pass
            return b

        ok_btn = mk(ok_label, ok_variant, self._on_ok)
        ok_btn.pack(side="right")
        if alt_label is not None:
            mk(alt_label, "secondary", self._on_alt).pack(
                side="right", padx=(0, px(4)))
        if cancel_label is not None:
            mk(cancel_label, "secondary", self._on_cancel).pack(
                side="right", padx=(0, px(4)))

        self.bind("<Return>", lambda _e: self._on_ok())
        self.bind("<Escape>", lambda _e: self._on_cancel())
        self.protocol("WM_DELETE_WINDOW", self._on_cancel)

        self.update_idletasks()
        try:
            want = max(px(480), frame.winfo_reqwidth())
            self.geometry("%dx%d" % (want, frame.winfo_reqheight() + 2))
        except Exception:
            pass
        frame.pack_configure(padx=1, pady=1)
        self._center_on(parent)
        self.deiconify()
        ok_btn.focus_set()
        try:
            self.grab_set()
        except Exception:
            pass
        self.wait_window(self)

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
            x0, y0 = top.winfo_rootx(), top.winfo_rooty()
            pw, ph = top.winfo_width(), top.winfo_height()
            x = x0 + max(0, (pw - w) // 2)
            y = y0 + max(0, (ph - h) // 3)
        except Exception:
            x = y = 200
        self.geometry("+%d+%d" % (x, y))


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
    """askyesno replacement — True if the primary button was pressed."""
    dlg = AppDialog(parent, title, message, variant="confirm", detail=detail,
                    note=note, ok_label=ok_label, cancel_label=cancel_label)
    return bool(dlg.result)


def app_danger(parent, title, message, *, detail=None, note=None,
               ok_label="Delete", cancel_label="Cancel"):
    """Destructive confirm (danger button) — True to proceed."""
    dlg = AppDialog(parent, title, message, variant="danger", detail=detail,
                    note=note, ok_label=ok_label, cancel_label=cancel_label,
                    ok_variant="danger")
    return bool(dlg.result)


def app_choose3(parent, title, message, *, detail=None, note=None,
                ok_label="Yes", alt_label="No", cancel_label="Cancel",
                variant="confirm"):
    """askyesnocancel replacement — True (primary), False (alt) or None."""
    dlg = AppDialog(parent, title, message, variant=variant, detail=detail,
                    note=note, ok_label=ok_label, alt_label=alt_label,
                    cancel_label=cancel_label)
    return dlg.result


def app_question(parent, title, message, *, detail=None, note=None,
                 yes_label="Yes", no_label="No", variant="confirm"):
    """askquestion replacement — returns 'yes' or 'no'."""
    dlg = AppDialog(parent, title, message, variant=variant, detail=detail,
                    note=note, ok_label=yes_label, cancel_label=no_label)
    return "yes" if dlg.result else "no"


__all__ = [
    "AppDialog", "app_info", "app_warning", "app_error", "app_success",
    "app_confirm", "app_danger", "app_choose3", "app_question",
]
