"""
ui/shared/scroll.py — one correct mousewheel-scroll implementation.

Background: most scrollable tabs bound <MouseWheel> directly to the Canvas
widget. That only delivers the event while the pointer is over BARE canvas —
any child widget (a Label, Frame, Combobox, Button…) sitting on top of the
canvas eats the wheel event, so scrolling feels dead or jumpy depending on
what the cursor is over. The Advanced tab already solved this by binding the
wheel GLOBALLY (bind_all) while the pointer is inside the pane and unbinding
on leave, so the wheel works no matter which child the cursor is over, while
stacked panes don't fight each other.

attach_scroll() factors that proven pattern into one helper so every tab
behaves identically.

Usage:
    canvas = tk.Canvas(parent, ...)
    inner  = tk.Frame(canvas, ...)
    canvas.create_window((0, 0), window=inner, anchor='nw')
    ...
    attach_scroll(canvas)            # that's it

Cross-platform: handles Windows/macOS <MouseWheel> (e.delta) and the X11
<Button-4>/<Button-5> wheel events.
"""

# Lines scrolled per wheel notch. The raw delta/120 == 1 unit/notch feels
# sluggish; 3 matches what most desktop apps do.
_SCROLL_SPEED = 3


_WHEEL_CARRY = {}


def wheel_units(widget, delta, speed=1):
    """Whole scroll units for a <MouseWheel> ``delta`` on ``widget``,
    keeping the fractional remainder per widget so the small deltas sent by
    precision touchpads add up instead of each truncating to zero."""
    key = id(widget)
    carry = _WHEEL_CARRY.get(key, 0.0) + (-1 * (delta / 120.0) * speed)
    units = int(carry)
    _WHEEL_CARRY[key] = carry - units
    return units


def attach_scroll(canvas, speed=_SCROLL_SPEED):
    """Make `canvas` scroll on the mousewheel anywhere the pointer is inside
    it (including over child widgets). Binds globally on <Enter>, releases on
    <Leave> so multiple scroll panes never fight. Safe/idempotent; never
    raises into the caller."""

    # Fractional scroll left over from small wheel deltas (see below).
    _carry = [0.0]

    def _on_wheel(event, c=canvas, s=speed):
        try:
            # Windows / macOS: event.delta is +/-120 per notch (mac is smaller
            # but same sign convention). Precision touchpads send many small
            # deltas (e.g. +/-15); truncating each one to whole units gave 0,
            # so touchpad scrolling did nothing. Accumulate the remainder.
            if getattr(event, 'delta', 0):
                _carry[0] += -1 * (event.delta / 120.0) * s
                units = int(_carry[0])
                if units:
                    _carry[0] -= units
                    c.yview_scroll(units, 'units')
            # X11: Button-4 = up, Button-5 = down (no delta).
            elif getattr(event, 'num', None) == 4:
                c.yview_scroll(-s, 'units')
            elif getattr(event, 'num', None) == 5:
                c.yview_scroll(s, 'units')
        except Exception:
            pass

    def _bind(_e=None, c=canvas):
        try:
            c.bind_all('<MouseWheel>', _on_wheel)     # Windows / macOS
            c.bind_all('<Button-4>', _on_wheel)        # X11 up
            c.bind_all('<Button-5>', _on_wheel)        # X11 down
        except Exception:
            pass

    def _unbind(_e=None, c=canvas):
        try:
            c.unbind_all('<MouseWheel>')
            c.unbind_all('<Button-4>')
            c.unbind_all('<Button-5>')
        except Exception:
            pass

    try:
        canvas.bind('<Enter>', _bind)
        canvas.bind('<Leave>', _unbind)
    except Exception:
        pass
    return canvas
