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


ELF = b'\x7fELF' + b'\0' * 2000


class _Inline:
    """threading.Thread stand-in that runs the target at start()."""
    def __init__(self, target=None, daemon=None, **_kw):
        self._t = target

    def start(self):
        self._t()


def _bare_app(mod, temp_dir):
    app = mod.ExFATBuilder.__new__(mod.ExFATBuilder)
    app._settings = {'temp_dir': temp_dir}
    app.after = lambda _ms, fn=None, *a: fn(*a) if fn else None
    app._log = lambda *_a, **_k: None
    app.logged = []
    app._abp_log_line = lambda text, tag='ok': app.logged.append((tag, text))
    app._bpr_log_entry = lambda *a: None
    app._sfo_zip_prefix = lambda folder, fallback='': 'PPSA01234 Game'
    app._apply_name_case = lambda name: name
    app.done = []
    app._abp_done = app.done.append

    class _Var:
        def set(self, *_a):
            pass
    app._abp_status_var = _Var()
    return app


class BackportRunSafety(unittest.TestCase):
    """The Auto Backport / Backport All plumbing around the engine: what
    is refused before anything changes, and what happens to the originals
    when a ZIP cannot be written."""

    def setUp(self):
        import shutil
        from unittest import mock
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='bp_run_')
        self.addCleanup(shutil.rmtree, self.d, True)
        self.game = os.path.join(self.d, 'PPSA01234-app')
        self.out = os.path.join(self.d, 'zips')
        self.tmp = os.path.join(self.d, 'temp')
        for p in (self.game, self.out, self.tmp,
                  os.path.join(self.game, 'sce_module')):
            os.makedirs(p, exist_ok=True)
        self._w('eboot.bin', ELF + b'orig')
        self._w('sce_module/libc.prx', ELF + b'libc')
        self.app = _bare_app(self.mod, self.tmp)
        p = mock.patch.object(self.mod.ExFATBuilder, '_read_sdk_from_folder',
                              staticmethod(lambda _f: 7))
        p.start()
        self.addCleanup(p.stop)
        p2 = mock.patch.object(self.mod, 'get_game_info',
                               lambda _f: ('Game', 'PPSA01234', '01.000'))
        p2.start()
        self.addCleanup(p2.stop)

    def _w(self, rel, data, base=None):
        p = os.path.join(base or self.game, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def _pre(self, sdk=4, fakelib=None, dest=None, pat=None):
        return self.app._abp_all_preflight(self.game, sdk, fakelib,
                                           dest or self.out, pat)

    def _temp_run(self):
        """A finished pipeline run: originals + patched copies in temp."""
        bk = tempfile.mkdtemp(prefix='exfat_builder_abp_', dir=self.tmp)
        o, p = os.path.join(bk, 'originals'), os.path.join(bk, 'patched')
        self._w('eboot.bin', ELF + b'orig', base=o)
        self._w('fakelib/libSceAgc.sprx', b'other tool', base=o)
        self._w('eboot.bin', ELF + b'patched', base=p)
        return bk, o, p

    def test_preflight_ok_and_refusals(self):
        self.assertIsNone(self._pre())
        self.assertIn('higher than the game', self._pre(sdk=8))
        self.assertIn('no originals ZIP folder', self.app._abp_all_preflight(
            self.game, 4, None, '', None))
        inside = os.path.join(self.game, 'backups')
        os.makedirs(inside)
        self.assertIn('inside the game folder', self._pre(dest=inside))
        self.assertIn('inside the game folder', self._pre(pat=inside))

    def test_preflight_fakelib(self):
        fl = os.path.join(self.d, 'fl')
        self._w('libSceAgc.sprx', ELF, base=fl)
        self.assertIsNone(self._pre(fakelib=fl))
        self._w('fakelib/libSceAgc.sprx', ELF, base=self.game)
        self.assertIn('already backported', self._pre(fakelib=fl))
        self.assertIn('inside the game', self._pre(
            fakelib=os.path.join(self.game, 'fakelib')))

    def test_second_run_refused_after_manifest(self):
        bk, o, p = self._temp_run()
        orig_zip, pat_zip, err = self.app._abp_all_save_zips(
            self.game, o, p, 'PPSA01234 Game', 4, self.out, self.out)
        self.assertIsNone(err)
        import zipfile
        with zipfile.ZipFile(orig_zip) as zf:
            # Another tool's fakelib file is part of the originals now.
            self.assertEqual(sorted(zf.namelist()),
                             ['eboot.bin', 'fakelib/libSceAgc.sprx'])
        self.assertTrue(os.path.basename(pat_zip).startswith('FW 4xx '))
        self.assertIsNone(self._pre())              # still the originals
        self._w('eboot.bin', ELF + b'patched')      # now what we produced
        self.assertIn('already backported', self._pre())
        self._w('eboot.bin', ELF + b'something else')
        self.assertIn('differ from the recorded originals', self._pre())

    def test_earlier_originals_zip_never_overwritten(self):
        first = os.path.join(self.out, 'PPSA01234 Game original files.zip')
        with open(first, 'wb') as f:
            f.write(b'the real originals')
        _bk, o, p = self._temp_run()
        orig_zip, _pz, err = self.app._abp_all_save_zips(
            self.game, o, p, 'PPSA01234 Game', 4, self.out, None)
        self.assertIsNone(err)
        self.assertTrue(orig_zip.endswith('original files (2).zip'))
        with open(first, 'rb') as f:
            self.assertEqual(f.read(), b'the real originals')

    def _zip_backups(self, dests, failed=0):
        from unittest import mock
        bk, o, p = self._temp_run()
        with mock.patch.object(self.mod.threading, 'Thread', _Inline):
            self.app._abp_zip_backups(o, p, 'PPSA01234-app', 4, self.game,
                                      dests=dests, failed=failed)
        return bk

    def test_zip_backups_success_removes_temp(self):
        bk = self._zip_backups((self.out, self.out))
        self.assertEqual(self.app.done, [True])
        self.assertFalse(os.path.exists(bk))
        names = sorted(os.listdir(self.out))
        self.assertIn('PPSA01234 Game original files.zip', names)
        self.assertIn('FW 4xx PPSA01234 Game backport files.zip', names)

    def test_zip_failure_is_reported_and_keeps_originals(self):
        gone = os.path.join(self.d, 'removed-after-check')
        bk = self._zip_backups((gone, None))
        self.assertEqual(self.app.done, [False])
        self.assertTrue(os.path.isfile(os.path.join(bk, 'originals', 'eboot.bin')))
        self.assertTrue(any(bk in t for _tag, t in self.app.logged))

    def test_pipeline_failures_are_not_success(self):
        self._zip_backups((self.out, None), failed=2)
        self.assertEqual(self.app.done, [False])

    def test_restore_refuses_zip_slip(self):
        import zipfile
        zp = os.path.join(self.out, 'evil.zip')
        with zipfile.ZipFile(zp, 'w') as zf:
            zf.writestr('eboot.bin', b'orig')
            zf.writestr('../../outside.txt', b'x')
        ok, n, err = self.app._abp_restore_one_zip(self.game, zp)
        self.assertFalse(ok)
        self.assertEqual(n, 1)
        self.assertIn('outside the game folder', err)
        self.assertFalse(os.path.exists(os.path.join(self.d, 'outside.txt')))


def _synthetic_elf(ps5_sdk=0x07000038):
    """A minimal 64-bit ELF with one PT_SCE_PROCPARAM segment, built here
    from the format (no game data): enough for the SDK patcher."""
    import struct
    b = bytearray(0x200)
    b[0:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHI', b, 0x10, 2, 0x3E, 1)
    struct.pack_into('<Q', b, 0x20, 0x40)
    struct.pack_into('<HH', b, 0x36, 0x38, 1)
    struct.pack_into('<IIQQQQQQ', b, 0x40, 0x61000001, 4, 0x100, 0, 0,
                     0x40, 0x40, 8)
    struct.pack_into('<IIIII', b, 0x100, 0x40, 0, 0x4942524F, 0x09590001,
                     ps5_sdk)
    return bytes(b)


class EngineInPlaceRegression(unittest.TestCase):
    """The embedded engine patches an ELF-only (decrypted) dump in place and
    leaves <file>.bak in the game. The originals must be copied before it
    runs, and the .bak files must not stay in the game folder."""

    def test_originals_snapshot_survives_in_place_patching(self):
        import io, shutil, sys, contextlib
        from core import backport_safety as bs
        mod = load_app()
        d = tempfile.mkdtemp(prefix='bp_inplace_')
        self.addCleanup(shutil.rmtree, d, True)
        game = os.path.join(d, 'game')
        os.makedirs(game)
        original = _synthetic_elf()
        with open(os.path.join(game, 'eboot.bin'), 'wb') as f:
            f.write(original)
        before = {'eboot.bin'}
        originals = os.path.join(d, 'originals')
        self.assertEqual(bs.snapshot_files(game, ['eboot.bin'], originals), 1)

        _bp, tmp = mod.extract_backport(d)
        saved_path, saved_mods = list(sys.path), dict(sys.modules)
        sys.path[:0] = [tmp, os.path.join(tmp, 'src')]
        try:
            from Backport import PS5ELFProcessor
            with contextlib.redirect_stdout(io.StringIO()):
                r = PS5ELFProcessor(use_colors=False).decrypt_and_sign_pipeline(
                    input_dir=game, output_dir=os.path.join(d, 'patched'),
                    sdk_pair=4, paid=0x3100000000000002, ptype=1,
                    fakelib_source=None, create_backup=True, overwrite=True,
                    apply_libc_patch=True, auto_revert_for_high_sdk=True,
                    verbose=True, save_to_config=False)
        finally:
            sys.path[:] = saved_path
            for k in set(sys.modules) - set(saved_mods):
                sys.modules.pop(k, None)
        self.assertEqual(r['downgrade']['successful'], 1)
        with open(os.path.join(game, 'eboot.bin'), 'rb') as f:
            in_place = f.read() != original
        # Measured with the embedded engine: the game file itself changes.
        self.assertTrue(in_place)
        # The snapshot taken before the run is the real original either way.
        with open(os.path.join(originals, 'eboot.bin'), 'rb') as f:
            self.assertEqual(f.read(), original)
        if in_place:
            self.assertTrue(os.path.isfile(os.path.join(game, 'eboot.bin.bak')))
        self.assertEqual(bs.remove_new_baks(game, before),
                         ['eboot.bin.bak'] if in_place else [])
        self.assertEqual(os.listdir(game), ['eboot.bin'])

    def test_remove_new_baks_keeps_preexisting(self):
        import shutil
        from core import backport_safety as bs
        d = tempfile.mkdtemp(prefix='bp_bak_')
        self.addCleanup(shutil.rmtree, d, True)
        for rel in ('eboot.bin.bak', 'sce_module/libc.prx.bak',
                    'save.bak', 'decrypted/eboot.bin.bak'):
            p = os.path.join(d, *rel.split('/'))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').close()
        removed = bs.remove_new_baks(d, {'eboot.bin.bak'})
        self.assertEqual(removed, ['sce_module/libc.prx.bak'])
        self.assertTrue(os.path.isfile(os.path.join(d, 'eboot.bin.bak')))
        self.assertTrue(os.path.isfile(os.path.join(d, 'save.bak')))


if __name__ == '__main__':
    unittest.main()
