"""Shared helpers for the test suite.

The application is a Windows Tkinter program. These helpers let its pure
logic be imported and tested on any OS without opening a window:

* on non-Windows hosts ``ctypes.windll`` is replaced by a harmless stub so the
  DPI-awareness and administrator checks that run at import time succeed;
* the module is imported under its normal name ``exfat_builder`` exactly once
  and cached for every test.

Nothing here touches real drives, OSFMount, robocopy or a PS5.
"""
import contextlib
import ctypes
import os
import sys
import tempfile
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)


class _WinDllStub:
    """Stands in for ``ctypes.windll`` off Windows: every attribute is another
    stub and every call returns 1 (``IsUserAnAdmin`` -> admin, DPI calls -> ok)."""

    def __getattr__(self, name):
        return _WinDllStub()

    def __call__(self, *args, **kwargs):
        return 1


def _install_windows_stubs():
    if os.name != 'nt' and not hasattr(ctypes, 'windll'):
        ctypes.windll = _WinDllStub()


_MODULE = None


def load_app():
    """Import and return the ``exfat_builder`` module (cached)."""
    global _MODULE
    if _MODULE is None:
        _install_windows_stubs()
        import exfat_builder  # noqa: E402  (import after stubbing)
        _MODULE = exfat_builder
    return _MODULE


def decode_embedded(name):
    """Return the decoded text of an embedded base64 script constant."""
    import base64
    mod = load_app()
    return base64.b64decode(getattr(mod, name)).decode('utf-8')


@contextlib.contextmanager
def temp_settings_file():
    """Redirect the app's settings file into a temporary folder."""
    mod = load_app()
    with tempfile.TemporaryDirectory(prefix='exfat_test_') as d:
        path = os.path.join(d, '.exfat_builder_settings.json')
        with mock.patch.object(mod, '_settings_path', lambda: path):
            yield path
