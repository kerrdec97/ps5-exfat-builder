"""Updater environment, worker output streams, bundled resources and
OSFMount discovery."""
import errno
import io
import os
import tempfile
import unittest
from unittest import mock

from tests._support import load_app


class UpdaterEnvironment(unittest.TestCase):
    def test_pyinstaller_private_state_is_removed(self):
        env = load_app().updater_environment({
            'PATH': r'C:\Windows',
            '_PYI_ARCHIVE_FILE': r'C:\old.exe',
            '_PYI_APPLICATION_HOME_DIR': r'C:\Temp\_MEI1234',
            '_MEIPASS2': r'C:\Temp\_MEI1234',
            'TCL_LIBRARY': r'C:\Temp\_MEI1234\tcl',
            'TK_LIBRARY': r'C:\Temp\_MEI1234\tk',
            'USERPROFILE': r'C:\Users\me',
        })
        self.assertEqual(env['PATH'], r'C:\Windows')
        self.assertEqual(env['USERPROFILE'], r'C:\Users\me')
        for gone in ('_PYI_ARCHIVE_FILE', '_PYI_APPLICATION_HOME_DIR',
                     '_MEIPASS2', 'TCL_LIBRARY', 'TK_LIBRARY'):
            self.assertNotIn(gone, env)
        self.assertEqual(env['PYINSTALLER_RESET_ENVIRONMENT'], '1')


class _BrokenPipe(io.StringIO):
    def write(self, data):
        raise OSError(errno.EINVAL, 'Invalid argument')


class WorkerStreams(unittest.TestCase):
    def test_einval_does_not_propagate(self):
        s = load_app()._TolerantStream(_BrokenPipe())
        self.assertEqual(s.write('Compressing 1 file...\n'), 22)
        self.assertEqual(s.write('more'), 4)      # stays quiet afterwards
        s.flush()

    def test_missing_stream(self):
        s = load_app()._TolerantStream(None)
        self.assertEqual(s.write('x'), 1)
        self.assertFalse(s.isatty())

    def test_healthy_stream_passes_through(self):
        buf = io.StringIO()
        s = load_app()._TolerantStream(buf)
        s.write('ok')
        self.assertEqual(buf.getvalue(), 'ok')


class ResourcePath(unittest.TestCase):
    def test_source_checkout_finds_bundled_files(self):
        app = load_app()
        for parts in (('CHANGELOG.md',), ('controller.ico',),
                      ('LICENSES', 'GPL-3.0.txt')):
            self.assertTrue(os.path.isfile(app.resource_path(*parts)), parts)

    def test_frozen_falls_back_to_bundle(self):
        app = load_app()
        with tempfile.TemporaryDirectory() as exe_dir, \
                tempfile.TemporaryDirectory() as bundle:
            with open(os.path.join(bundle, 'CHANGELOG.md'), 'w') as f:
                f.write('# Changelog')
            with mock.patch.object(app.sys, 'frozen', True, create=True), \
                    mock.patch.object(app.sys, '_MEIPASS', bundle, create=True), \
                    mock.patch.object(app.sys, 'executable',
                                      os.path.join(exe_dir, 'app.exe')):
                self.assertEqual(app.resource_path('CHANGELOG.md'),
                                 os.path.join(bundle, 'CHANGELOG.md'))
                # A copy next to the exe overrides the bundled one.
                override = os.path.join(exe_dir, 'CHANGELOG.md')
                with open(override, 'w') as f:
                    f.write('# newer')
                self.assertEqual(app.resource_path('CHANGELOG.md'), override)


class OsfmountDiscovery(unittest.TestCase):
    """Issue #86: a lone osfmount.exe (PassMark's installer has that name)
    saved as the custom path must not beat a real installation."""

    def setUp(self):
        self.app = load_app()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.downloads = os.path.join(self.tmp.name, 'Downloads')
        os.makedirs(self.downloads)
        self.installer = os.path.join(self.downloads, 'osfmount.exe')
        open(self.installer, 'wb').close()
        self.pf = os.path.join(self.tmp.name, 'Program Files')

    def _find(self):
        fake = type('F', (), {})()
        fake._settings = {'osfmount_path': self.installer}
        env = {'ProgramFiles': self.pf, 'ProgramFiles(x86)': self.pf,
               'ProgramW6432': self.pf, 'PATH': ''}
        with mock.patch.dict(os.environ, env, clear=False):
            return self.app.ExFATBuilder._find_osfmount(fake)

    def test_real_install_wins_over_lone_exe(self):
        inst = os.path.join(self.pf, 'OSFMount')
        os.makedirs(inst)
        com = os.path.join(inst, 'osfmount.com')
        open(com, 'wb').close()
        self.assertEqual(os.path.normcase(self._find()), os.path.normcase(com))

    def test_lone_exe_still_used_when_nothing_else_exists(self):
        self.assertEqual(self._find(), self.installer)

    def test_custom_com_is_used_directly(self):
        com = os.path.join(self.downloads, 'osfmount.com')
        open(com, 'wb').close()
        self.assertEqual(self._find(), com)


if __name__ == '__main__':
    unittest.main()


class ImageSpaceCheck(unittest.TestCase):
    """Issue #33: adding files to a full image failed file-by-file."""

    def test_shortfall_reported(self):
        app = load_app()
        with tempfile.TemporaryDirectory() as d:
            free = __import__('shutil').disk_usage(d).free
            self.assertIsNone(app.image_space_shortfall(d, 1024))
            need, have = app.image_space_shortfall(d, free + 10 ** 9)
            self.assertGreater(need, have)
            # Overwritten files give their space back.
            self.assertIsNone(app.image_space_shortfall(d, free + 10 ** 9,
                                                        sizes_replaced=10 ** 9 + 2 ** 23))
            self.assertIn('fixed size', app.image_full_message(need, have))


class DotnetHint(unittest.TestCase):
    """Closed issue #70: 'newfs failed (rc=2147516547)'."""

    def test_codes_and_text(self):
        hint = load_app().dotnet_runtime_hint
        self.assertIn('.NET 8', hint(2147516547))          # 0x80008083 unsigned
        self.assertIn('.NET 8', hint(-2147450749))         # same, signed
        self.assertIn('.NET 8', hint(1, 'Failed to resolve hostfxr.dll [not found]'))
        self.assertEqual(hint(1, 'Error: File too large'), '')
        self.assertEqual(hint(0), '')
