"""core/backport_safety.py: fakelib/destination checks, atomic ZIPs,
zip-slip-safe restore and manifest-based already-backported detection."""
import os
import shutil
import tempfile
import unittest
import zipfile

from core import backport_safety as bs

ELF = b'\x7fELF' + b'\0' * 2000


def _is_exec(path):
    with open(path, 'rb') as f:
        return f.read(4) in (b'\x7fELF', b'\x4f\x15\x3d\x1d', b'\x54\x14\xf5\xee')


class _Tmp(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix='bp_safety_')
        self.addCleanup(shutil.rmtree, self.d, True)

    def put(self, rel, data=ELF):
        p = os.path.join(self.d, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
        return p


class FakelibSource(_Tmp):
    def test_good_set(self):
        self.put('set/libSceAgc.sprx')
        self.put('set/readme.txt', b'notes')
        self.put('set/._libSceAgc.sprx', b'\x00\x05\x16\x07')
        self.assertEqual(bs.validate_fakelib_source(
            os.path.join(self.d, 'set'), _is_exec), [])

    def test_nested_set_rejected(self):
        # The user picked the folder above the set: files would land in
        # game/fakelib/fakelib/ where they are never loaded.
        self.put('parent/fakelib/libSceAgc.sprx')
        problems = bs.validate_fakelib_source(
            os.path.join(self.d, 'parent'), _is_exec)
        self.assertEqual(len(problems), 1)
        self.assertIn('sub-folder', problems[0])

    def test_undersized_and_non_elf_rejected(self):
        self.put('set/libSceAgc.sprx', b'')
        self.put('set/libSceA.sprx', b'version https://git-lfs.github.com/spec/v1\n')
        self.put('set/libSceB.prx', b'MZ' + b'\0' * 4000)
        problems = bs.validate_fakelib_source(
            os.path.join(self.d, 'set'), _is_exec)
        self.assertEqual(len(problems), 3, problems)
        self.assertTrue(any('0 bytes' in p for p in problems))
        self.assertTrue(any('not an ELF/SELF' in p for p in problems))

    def test_empty_or_missing(self):
        os.makedirs(os.path.join(self.d, 'empty'))
        self.assertIn('no .sprx/.prx', bs.validate_fakelib_source(
            os.path.join(self.d, 'empty'), _is_exec)[0])
        self.assertIn('does not exist', bs.validate_fakelib_source(
            os.path.join(self.d, 'nope'), _is_exec)[0])

    def test_inside_game_rejected(self):
        self.put('game/eboot.bin')
        self.put('game/fakelib/libSceAgc.sprx')
        p = bs.validate_fakelib_source(os.path.join(self.d, 'game', 'fakelib'),
                                       _is_exec, game=os.path.join(self.d, 'game'))
        self.assertIn('outside the game folder', p[0])


class ZipDestination(_Tmp):
    def setUp(self):
        super().setUp()
        self.game = os.path.dirname(self.put('game/eboot.bin'))
        self.out = os.path.join(self.d, 'out')
        os.makedirs(self.out)

    def test_ok(self):
        self.assertIsNone(bs.validate_zip_destination(
            self.out, self.game, 'PPSA01234 original files.zip'))
        self.assertEqual(os.listdir(self.out), [])   # probe removed

    def test_inside_game_rejected(self):
        for dest in (self.game, os.path.join(self.game, 'sce_sys')):
            os.makedirs(dest, exist_ok=True)
            self.assertIn('inside the game folder', bs.validate_zip_destination(
                dest, self.game, 'x.zip'))

    def test_sibling_with_common_prefix_allowed(self):
        # "game2" starts with "game" but is not inside it.
        sib = os.path.join(self.d, 'game2')
        os.makedirs(sib)
        self.assertIsNone(bs.validate_zip_destination(sib, self.game, 'x.zip'))

    def test_missing_and_long(self):
        self.assertIn('does not exist', bs.validate_zip_destination(
            os.path.join(self.d, 'nope'), self.game, 'x.zip'))
        self.assertIn('No folder', bs.validate_zip_destination('', self.game, 'x.zip'))
        msg = bs.validate_zip_destination(self.out, self.game, 'x' * 250 + '.zip')
        self.assertIn('Windows only guarantees 259', msg)

    def test_temp_path_length(self):
        self.assertIsNone(bs.temp_path_problem(self.d, self.game, ['eboot.bin']))
        self.assertIn('temp folder', bs.temp_path_problem(
            self.d, self.game, ['eboot.bin', 'a/' * 120 + 'x.prx']))


class AtomicZip(_Tmp):
    def test_writes_and_never_overwrites(self):
        src = os.path.dirname(self.put('src/eboot.bin'))
        self.put('src/sce_module/libc.prx')
        target = os.path.join(self.d, 'G original files.zip')
        with open(target, 'wb') as f:
            f.write(b'earlier originals')
        dest = bs.unique_path(target)
        self.assertTrue(dest.endswith('G original files (2).zip'))
        n = bs.write_zip_atomic(dest, bs.dir_members(src))
        self.assertEqual(n, 2)
        with open(target, 'rb') as f:
            self.assertEqual(f.read(), b'earlier originals')
        with zipfile.ZipFile(dest) as zf:
            self.assertEqual(sorted(zf.namelist()),
                             ['eboot.bin', 'sce_module/libc.prx'])
        self.assertFalse(os.path.exists(dest + '.part'))

    def test_failure_leaves_nothing(self):
        dest = os.path.join(self.d, 'x.zip')
        with self.assertRaises(OSError):
            bs.write_zip_atomic(dest, [(os.path.join(self.d, 'missing'), 'a')])
        self.assertEqual(os.listdir(self.d), [])

    def test_zip_handles_closed(self):
        # On Windows an open handle would block this rename and delete.
        src = os.path.dirname(self.put('src/eboot.bin'))
        dest = os.path.join(self.d, 'x.zip')
        bs.write_zip_atomic(dest, bs.dir_members(src))
        os.rename(dest, dest + '.moved')
        os.remove(dest + '.moved')
        zp = os.path.join(self.d, 'r.zip')
        with zipfile.ZipFile(zp, 'w') as zf:
            zf.writestr('eboot.bin', b'x')
        bs.restore_zip(zp, os.path.join(self.d, 'g'))
        os.remove(zp)


class Restore(_Tmp):
    def test_zip_slip_refused(self):
        game = os.path.join(self.d, 'game')
        os.makedirs(game)
        zp = os.path.join(self.d, 'o.zip')
        with zipfile.ZipFile(zp, 'w') as zf:
            zf.writestr('eboot.bin', b'orig')
            zf.writestr('sce_module/libc.prx', b'libc')
            zf.writestr('../escape.txt', b'bad')
            zf.writestr('/abs.txt', b'bad')
            zf.writestr('C:/win.txt', b'bad')
            zf.writestr('sub\\..\\..\\evil.txt', b'bad')
        restored, errors = bs.restore_zip(zp, game)
        self.assertEqual(restored, 2)
        self.assertEqual(len(errors), 4)
        self.assertFalse(os.path.exists(os.path.join(self.d, 'escape.txt')))
        self.assertFalse(os.path.exists(os.path.join(self.d, 'evil.txt')))
        with open(os.path.join(game, 'eboot.bin'), 'rb') as f:
            self.assertEqual(f.read(), b'orig')

    def test_safe_member_target(self):
        root = self.d
        self.assertEqual(bs.safe_member_target(root, 'a/b.bin'),
                         os.path.join(os.path.abspath(root), 'a', 'b.bin'))
        for bad in ('', '/', '../x', 'a/../../x', 'D:x', '\\\\srv\\s\\x'):
            self.assertIsNone(bs.safe_member_target(root, bad), bad)


class ManifestDetection(_Tmp):
    def setUp(self):
        super().setUp()
        self.game = os.path.dirname(self.put('game/eboot.bin', b'patched'))
        self.put('game/sce_module/libc.prx', b'libc-patched')
        self.orig = {'eboot.bin': bs.sha256_of(self.put('o/eboot.bin', b'orig')),
                     'sce_sys/param.json': 'ignored',
                     'decrypted/eboot.bin': 'ignored'}

    def test_patched_hashes_match(self):
        post = bs.tree_hashes(self.game)
        m = {'originals': {'files': self.orig},
             'targets': {'4xx': {'post_hashes': post}}}
        verdict, files = bs.backported_by_manifest(self.game, m)
        self.assertEqual(verdict, 'patched')
        self.assertIn('eboot.bin', files)

    def test_restored_originals_are_original(self):
        self.put('game/eboot.bin', b'orig')
        m = {'originals': {'files': self.orig},
             'targets': {'4xx': {'post_hashes': {'eboot.bin': 'aa'}}}}
        self.assertEqual(bs.backported_by_manifest(self.game, m)[0], 'original')

    def test_old_manifest_without_post_hashes(self):
        m = {'originals': {'files': self.orig}, 'targets': {'4xx': {}}}
        self.assertEqual(bs.backported_by_manifest(self.game, m),
                         ('differs', ['eboot.bin']))
        self.assertEqual(bs.backported_by_manifest(self.game, None)[0], 'unknown')

    def test_tree_hashes_strips_wrapper(self):
        self.put('p/game/eboot.bin', b'x')
        self.put('p/game/decrypted/eboot.bin', b'y')
        h = bs.tree_hashes(os.path.join(self.d, 'p'), strip_first='game')
        self.assertEqual(list(h), ['eboot.bin'])


if __name__ == '__main__':
    unittest.main()
