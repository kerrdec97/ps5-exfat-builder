"""Backport engine: which files are treated as patchable executables."""
import os
import tempfile
import unittest

from tests._support import load_app


class ExecutableDetection(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='bp_exec_')

    def _write(self, rel, data):
        p = os.path.join(self.d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def test_real_executables_detected(self):
        is_exec = self.mod.ExFATBuilder._abp_is_executable_file
        self.assertTrue(is_exec(self._write('eboot.bin', b'\x7fELF' + b'\0' * 60)))
        self.assertTrue(is_exec(self._write('sce_module/libc.prx', b'\x4f\x15\x3d\x1d' + b'\0' * 60)))
        self.assertTrue(is_exec(self._write('a.sprx', b'\x54\x14\xf5\xee' + b'\0' * 60)))

    def test_os_metadata_is_never_an_executable(self):
        is_exec = self.mod.ExFATBuilder._abp_is_executable_file
        appledouble = b'\x00\x05\x16\x07' + b'\0' * 60
        self.assertFalse(is_exec(self._write('._libc.prx', appledouble)))
        self.assertFalse(is_exec(self._write('._eboot.bin', appledouble)))
        self.assertFalse(is_exec(self._write('.DS_Store', b'\0\0\0\1Bud1')))
        self.assertFalse(is_exec(self._write('Thumbs.db', b'\xd0\xcf\x11\xe0')))

    def test_readable_non_elf_with_executable_extension(self):
        is_exec = self.mod.ExFATBuilder._abp_is_executable_file
        self.assertFalse(is_exec(self._write('notes.prx', b'hello world')))

    def test_scan_skips_metadata_dirs(self):
        self._write('eboot.bin', b'\x7fELF' + b'\0' * 60)
        self._write('__MACOSX/sce_module/._libc.prx', b'\x00\x05\x16\x07')
        self._write('sce_module/._libc.prx', b'\x00\x05\x16\x07')
        self._write('sce_module/libc.prx', b'\x7fELF' + b'\0' * 60)
        app = self.mod.ExFATBuilder.__new__(self.mod.ExFATBuilder)
        execs, _skipped = self.mod.ExFATBuilder._abp_scan_executables(app, self.d)
        self.assertEqual(sorted(e.replace('\\', '/') for e in execs),
                         ['eboot.bin', 'sce_module/libc.prx'])


class FakelibState(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='bp_fl_')
        self.addCleanup(__import__('shutil').rmtree, self.d, True)
        self.game = os.path.join(self.d, 'game')
        self.src = os.path.join(self.d, 'fakelib_set')
        for base in (self.game, self.src):
            os.makedirs(base)
        open(os.path.join(self.game, 'eboot.bin'), 'wb').close()
        with open(os.path.join(self.src, 'libSceAgc.sprx'), 'wb') as f:
            f.write(b'agc' * 100)

    def _put(self, data):
        os.makedirs(os.path.join(self.game, 'fakelib'), exist_ok=True)
        with open(os.path.join(self.game, 'fakelib', 'libSceAgc.sprx'), 'wb') as f:
            f.write(data)

    def test_states(self):
        st = self.mod.backport_fakelib_state
        self.assertEqual(st(self.game, self.src), 'none')
        self._put(b'other tool')
        self.assertEqual(st(self.game, self.src), 'other')
        self._put(b'agc' * 100)
        self.assertEqual(st(self.game, self.src), 'same')
        self.assertEqual(st(self.game, None), 'other')


class ExtractBackport(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.base = tempfile.mkdtemp(prefix='bp_base_')
        self.addCleanup(__import__('shutil').rmtree, self.base, True)

    def test_private_fresh_folder_each_time(self):
        import sys
        # A stale Backport.py and a planted module in the user's Temp root
        # must never be used.
        with open(os.path.join(self.base, 'Backport.py'), 'w') as f:
            f.write('raise SystemExit("stale")\n')
        bp1, tmp1 = self.mod.extract_backport(self.base)
        self.assertNotEqual(os.path.normcase(tmp1), os.path.normcase(self.base))
        self.assertEqual(os.path.dirname(tmp1), self.base)
        with open(bp1, 'rb') as f:
            self.assertNotIn(b'stale', f.read(200))
        self.assertTrue(os.path.isfile(os.path.join(tmp1, 'src', 'make_fself.py')))
        sys.path.insert(0, tmp1)
        bp2, tmp2 = self.mod.extract_backport(self.base)
        self.assertNotEqual(tmp1, tmp2)
        self.assertFalse(os.path.exists(tmp1))      # previous one cleaned up
        self.assertNotIn(tmp1, sys.path)
        self.assertNotIn(self.base, sys.path)
        __import__('shutil').rmtree(tmp2, True)


if __name__ == '__main__':
    unittest.main()
