"""Mouse-wheel / touchpad scrolling helper (follow-up to issue #39)."""
import os
import unittest

from tests._support import load_app


class _FakeCanvas:
    def __init__(self):
        self.scrolled = 0
        self.bindings = {}

    def yview_scroll(self, n, what):
        self.scrolled += n

    def bind(self, seq, fn):
        self.bindings[seq] = fn

    def bind_all(self, seq, fn=None):
        self.bindings['all' + seq] = fn

    def unbind_all(self, seq):
        self.bindings.pop('all' + seq, None)


class _Ev:
    def __init__(self, delta=0, num=None):
        self.delta = delta
        self.num = num


class WheelScrolling(unittest.TestCase):
    def setUp(self):
        load_app()
        from ui.shared.scroll import attach_scroll
        self.c = _FakeCanvas()
        attach_scroll(self.c, speed=3)
        self.c.bindings['<Enter>']()
        self.wheel = self.c.bindings['all<MouseWheel>']

    def test_mouse_notch(self):
        self.wheel(_Ev(delta=-120))
        self.assertEqual(self.c.scrolled, 3)

    def test_precision_touchpad_small_deltas_add_up(self):
        for _ in range(8):           # 8 x 15 = one full notch
            self.wheel(_Ev(delta=-15))
        self.assertEqual(self.c.scrolled, 3)

    def test_x11_buttons(self):
        self.c.bindings['all<Button-5>'](_Ev(num=5))
        self.c.bindings['all<Button-4>'](_Ev(num=4))
        self.assertEqual(self.c.scrolled, 0)


if __name__ == '__main__':
    unittest.main()


class WheelUnits(unittest.TestCase):
    def test_carry_per_widget(self):
        load_app()
        from ui.shared.scroll import wheel_units
        a, b = object(), object()
        self.assertEqual(wheel_units(a, -120), 1)
        got = [wheel_units(b, -15) for _ in range(8)]
        self.assertEqual(sum(got), 1)
        self.assertEqual(wheel_units(a, 240), -2)
