"""Data-safety rules: when an output counts as finished and when a source
may be deleted (technical audit C1, C2, H1, H2, H6)."""
import os
import re
import shutil
import sys
import tempfile
import unittest

from tests._support import load_app, ROOT


def _write(base, rel, data=b''):
    p = os.path.join(base, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(data)
    return p


class StrictImageComparison(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()

    def test_identical_trees_match(self):
        src = {'eboot.bin': 100, 'sce_sys/param.json': 10}
        ok, problems, n = self.mod.image_matches_source(src, dict(src))
        self.assertTrue(ok)
        self.assertEqual((problems, n), ([], 0))

    def test_one_small_missing_file_fails(self):
        # 0.5 % of a 50 GB game is 250 MB: the old tolerance let a missing
        # file through. The strict rule must not.
        src = {'eboot.bin': 50 * 1024**3, 'sce_module/libc.prx': 1024}
        img = {'eboot.bin': 50 * 1024**3}
        ok, problems, n = self.mod.image_matches_source(src, img)
        self.assertFalse(ok)
        self.assertEqual(n, 1)
        self.assertIn('libc.prx', problems[0])

    def test_short_file_and_missing_empty_file_fail(self):
        src = {'a.bin': 10, 'empty.txt': 0}
        ok, _p, n = self.mod.image_matches_source(src, {'a.bin': 9})
        self.assertFalse(ok)
        self.assertEqual(n, 2)

    def test_extra_files_on_image_are_fine(self):
        ok, _p, _n = self.mod.image_matches_source(
            {'a': 1}, {'a': 1, 'System Volume Information/x': 5})
        self.assertTrue(ok)


class SourceExclusions(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='excl_')
        self.addCleanup(shutil.rmtree, self.d, True)
        _write(self.d, 'eboot.bin', b'x')

    def test_plain_dump_has_no_exclusions(self):
        self.assertFalse(self.mod.source_has_build_exclusions(self.d))

    def test_decrypted_folder_and_nested_images_are_exclusions(self):
        _write(self.d, 'decrypted/eboot.bin', b'x')
        self.assertTrue(self.mod.source_has_build_exclusions(self.d))
        shutil.rmtree(os.path.join(self.d, 'decrypted'))
        _write(self.d, 'old/build.exfat', b'x')
        self.assertTrue(self.mod.source_has_build_exclusions(self.d))


class FfpkgPartFiles(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='ffpkg_')
        self.addCleanup(shutil.rmtree, self.d, True)
        self.out = os.path.join(self.d, 'Game.ffpkg')
        self.part = self.mod.ffpkg_part_path(self.out)

    def test_success_renames_part(self):
        _write(self.d, os.path.basename(self.part), b'U' * 4096)
        rc, msg = self.mod.finalize_ffpkg_part(0, self.out)
        self.assertEqual((rc, msg), (0, ''))
        self.assertTrue(os.path.isfile(self.out))
        self.assertFalse(os.path.exists(self.part))

    def test_success_replaces_older_image_only_now(self):
        _write(self.d, 'Game.ffpkg', b'old')
        _write(self.d, os.path.basename(self.part), b'new image')
        self.mod.finalize_ffpkg_part(0, self.out)
        with open(self.out, 'rb') as f:
            self.assertEqual(f.read(), b'new image')

    def test_failure_removes_part_and_keeps_older_image(self):
        _write(self.d, 'Game.ffpkg', b'old good image')
        _write(self.d, os.path.basename(self.part), b'half')
        rc, msg = self.mod.finalize_ffpkg_part(3, self.out)
        self.assertEqual(rc, 3)
        self.assertIn('incomplete', msg.lower())
        self.assertFalse(os.path.exists(self.part))
        with open(self.out, 'rb') as f:
            self.assertEqual(f.read(), b'old good image')

    def test_zero_exit_without_output_is_a_failure(self):
        rc, msg = self.mod.finalize_ffpkg_part(0, self.out)
        self.assertNotEqual(rc, 0)
        self.assertIn('no image', msg)


class FfpkgDeleteSource(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='ffdel_')
        self.addCleanup(shutil.rmtree, self.d, True)
        self.src = os.path.join(self.d, 'src')
        _write(self.src, 'eboot.bin', b'e' * 1000)
        _write(self.src, 'sce_sys/param.json', b'{}')
        self.out = os.path.join(self.d, 'Game.ffpkg')

    def test_image_smaller_than_source_keeps_source(self):
        _write(self.d, 'Game.ffpkg', b'u' * 10)
        ok, why = self.mod.ffpkg_safe_to_delete_source(self.src, self.out)
        self.assertFalse(ok)
        self.assertIn('smaller', why)

    def test_complete_image_allows_delete(self):
        _write(self.d, 'Game.ffpkg', b'u' * 65536)
        ok, _why = self.mod.ffpkg_safe_to_delete_source(self.src, self.out)
        self.assertTrue(ok)

    def test_missing_image_keeps_source(self):
        ok, _why = self.mod.ffpkg_safe_to_delete_source(self.src, self.out)
        self.assertFalse(ok)


class MkpfsExitCodes(unittest.TestCase):
    """mkpfs returns 1 after writing an image whose post-create check found
    errors. No PFS path may treat that as success (audit C1)."""

    def test_no_pfs_path_accepts_rc_1(self):
        for name in ('ui/tab_pfs.py', 'ui/tab_unibuild.py', 'exfat_builder.py'):
            with open(os.path.join(ROOT, name), encoding='utf-8') as f:
                src = f.read()
            self.assertIsNone(
                re.search(r'rc\d?\s*==\s*1\s+and', src),
                '%s accepts mkpfs exit code 1 as success' % name)


class RebuildKeepsPreviousImage(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='prev_')
        self.addCleanup(shutil.rmtree, self.d, True)
        self.out = os.path.join(self.d, 'Game.exfat')

    def _read(self, p):
        with open(p, 'rb') as f:
            return f.read()

    def test_nothing_to_set_aside(self):
        self.assertIsNone(self.mod.set_aside_previous_image(self.out))
        self.assertEqual(self.mod.settle_previous_image(self.out, None, False), '')

    def test_success_drops_previous(self):
        _write(self.d, 'Game.exfat', b'old')
        aside = self.mod.set_aside_previous_image(self.out)
        self.assertFalse(os.path.exists(self.out))
        _write(self.d, 'Game.exfat', b'new')
        self.mod.settle_previous_image(self.out, aside, success=True)
        self.assertEqual(self._read(self.out), b'new')
        self.assertFalse(os.path.exists(aside))

    def test_failure_restores_previous_and_keeps_failed(self):
        _write(self.d, 'Game.exfat', b'old good')
        aside = self.mod.set_aside_previous_image(self.out)
        _write(self.d, 'Game.exfat', b'half new')
        msg = self.mod.settle_previous_image(self.out, aside, success=False)
        self.assertIn('restored', msg)
        self.assertEqual(self._read(self.out), b'old good')
        self.assertEqual(self._read(os.path.join(self.d, 'Game.failed.exfat')),
                         b'half new')

    def test_failure_after_script_deleted_new_image(self):
        _write(self.d, 'Game.exfat', b'old good')
        aside = self.mod.set_aside_previous_image(self.out)
        self.mod.settle_previous_image(self.out, aside, success=False)
        self.assertEqual(self._read(self.out), b'old good')


class QueueSummary(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()

    def test_counts_real_outcomes(self):
        t, msg, ok = self.mod.summarize_outcomes(['done', 'failed', 'warn'])
        self.assertFalse(ok)
        self.assertIn('1 image(s) built successfully', msg)
        self.assertIn('1 failed', msg)
        self.assertIn('1 built with a verification warning', msg)
        self.assertEqual(t, 'Finished with problems')

    def test_all_failed_is_not_reported_as_success(self):
        t, msg, ok = self.mod.summarize_outcomes(['failed', 'failed'])
        self.assertFalse(ok)
        self.assertEqual(t, 'Build failed')
        self.assertIn('0 image(s) built successfully', msg)

    def test_all_done(self):
        t, msg, ok = self.mod.summarize_outcomes(['done', 'done'])
        self.assertTrue(ok)
        self.assertEqual(t, 'Complete')


class CoverArtFormats(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='cover_')
        self.addCleanup(shutil.rmtree, self.d, True)

    def test_png_cover_accepted_other_formats_refused(self):
        from PIL import Image
        good = os.path.join(self.d, 'icon0.png')
        Image.new('RGB', (64, 96), (200, 30, 30)).save(good, 'PNG')
        self.assertTrue(self.mod._normalize_cover(good))
        bad = os.path.join(self.d, 'pic0.png')       # a GIF named .png
        Image.new('RGB', (64, 96), (30, 200, 30)).save(bad, 'GIF')
        self.assertIsNone(self.mod._normalize_cover(bad))


class ChildProcesses(unittest.TestCase):
    def test_kill_child_tree(self):
        try:
            import psutil  # noqa: F401
        except ImportError:
            self.skipTest('psutil not installed')
        import subprocess
        import time
        mod = load_app()
        # A child that starts a grandchild, both sleeping.
        code = ('import subprocess, sys, time; '
                'subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"]); '
                'time.sleep(60)')
        p = subprocess.Popen([sys.executable, '-c', code])
        try:
            import psutil
            deadline = time.time() + 10
            while time.time() < deadline and not psutil.Process(p.pid).children():
                time.sleep(0.05)
            grand = psutil.Process(p.pid).children()
            self.assertTrue(grand)
            n = mod.kill_child_processes(timeout=5)
            self.assertGreaterEqual(n, 2)
            p.wait(timeout=10)
            for g in grand:
                self.assertFalse(g.is_running() and g.status() != psutil.STATUS_ZOMBIE)
        finally:
            if p.poll() is None:
                p.kill()


class DriveLetters(unittest.TestCase):
    def test_network_letters_marked_used(self):
        from unittest import mock
        load_app()
        from ui.shared import drives
        with mock.patch.object(drives, 'network_mapped_letters',
                               return_value={'Z', 'm'.upper()}):
            mask = drives.used_letter_mask()
        self.assertTrue(mask & (1 << 25))           # Z:
        self.assertTrue(mask & (1 << 12))           # M:

    def test_no_crash_off_windows(self):
        load_app()
        from ui.shared import drives
        self.assertIsInstance(drives.network_mapped_letters(), set)
        self.assertIsInstance(drives.used_letter_mask(), int)


class SettingsAndHistoryFiles(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.d = tempfile.mkdtemp(prefix='hist_')
        self.addCleanup(shutil.rmtree, self.d, True)

    def test_non_object_settings_set_aside(self):
        from tests._support import temp_settings_file
        with temp_settings_file() as path:
            with open(path, 'w') as f:
                f.write('[1, 2]')
            self.assertEqual(self.mod.load_settings(), {})
            self.assertTrue(os.path.isfile(path + '.corrupt'))

    def _fake(self):
        hp = os.path.join(self.d, 'history.json')

        class Fake:
            def _history_path(self_inner):
                return hp

        class Item:
            game_title = 'Game'
            game_folder = self.d
            title_id = 'PPSA00001'
            version = '01.000'
        return Fake(), Item(), hp

    def test_history_written_atomically_and_corrupt_kept(self):
        import json
        fake, item, hp = self._fake()
        with open(hp, 'w') as f:
            f.write('{not json')
        self.mod.ExFATBuilder._save_history(fake, item, 'x.exfat', True)
        self.assertTrue(os.path.isfile(hp + '.corrupt'))
        with open(hp) as f:
            data = json.load(f)
        self.assertEqual(len(data), 1)
        self.assertTrue(data[0]['success'])
        self.assertFalse(os.path.exists(hp + '.tmp'))
        self.mod.ExFATBuilder._save_history(fake, item, 'y.exfat', False)
        with open(hp) as f:
            self.assertEqual([e['success'] for e in json.load(f)], [False, True])


class StaleMountSweep(unittest.TestCase):
    def test_sweep_leaves_volumes_mounted_before_start(self):
        import ctypes
        from unittest import mock
        mod = load_app()

        class K32:
            @staticmethod
            def GetLogicalDrives():
                return (1 << 2) | (1 << 7) | (1 << 8) | (1 << 9)  # C H I J

        class WinDll:
            kernel32 = K32()

        vols = {'H': ('PS5DATA', 111), 'I': ('PS5DATA', 222), 'J': ('DATA', 3)}
        dismounted = []

        class Fake:
            _preexisting_osf_volumes = {('H', 111), ('I', 999)}

            def _log(self, *_a):
                pass

            def _dismount_drive_robust(self, letter, max_wait_seconds=15):
                dismounted.append(letter)
                return True

        with mock.patch.object(ctypes, 'windll', WinDll(), create=True), \
                mock.patch.object(mod, '_volume_label_serial',
                                  lambda l: vols.get(l, ('', None))):
            killed = mod.ExFATBuilder._sweep_stale_osf_mounts(Fake())
        # H was mounted before start (same serial): kept. I has a new
        # serial (remounted since): swept. J is not an app label: kept.
        self.assertEqual(killed, ['I'])
        self.assertEqual(dismounted, ['I'])


class BackgroundDelete(unittest.TestCase):
    def test_rmtree_runs_off_the_calling_thread(self):
        import threading
        import time
        mod = load_app()
        d = tempfile.mkdtemp(prefix='rmbg_')
        _write(d, 'a/b/c.bin', b'x')
        seen = []
        mod.ExFATBuilder._rmtree_in_background(
            object(), d, lambda t: seen.append((threading.get_ident(), t)))
        deadline = time.time() + 10
        while os.path.exists(d) and time.time() < deadline:
            time.sleep(0.02)
        self.assertFalse(os.path.exists(d))
        while len(seen) < 2 and time.time() < deadline:
            time.sleep(0.02)
        self.assertTrue(all(tid != threading.get_ident() for tid, _t in seen))
        self.assertIn('deleted', seen[-1][1])


class ExtractedToolIntegrity(unittest.TestCase):
    def test_scripts_and_ufs2tool_rechecked(self):
        mod = load_app()
        d = tempfile.mkdtemp(prefix='integ_')
        self.addCleanup(shutil.rmtree, d, True)
        bat, _ico = mod.extract_scripts(d)
        self.addCleanup(mod.cleanup_scripts)
        self.assertTrue(mod.scripts_intact(bat))
        ps1 = os.path.join(os.path.dirname(bat), 'New-OsfExfatImage.ps1')
        with open(ps1, 'ab') as f:
            f.write(b'\nWrite-Host injected\n')
        self.assertFalse(mod.scripts_intact(bat))
        self.assertFalse(mod.scripts_intact(None))
        exe = mod.extract_ufs2tool(d)
        self.assertTrue(mod.ufs2tool_intact(exe))
        with open(os.path.join(os.path.dirname(exe), 'DokanNet.dll'), 'r+b') as f:
            f.write(b'MZ!!')
        self.assertFalse(mod.ufs2tool_intact(exe))


class DotnetRuntime(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()

    def test_dotnet_8_and_newer_accepted(self):
        ok = self.mod.dotnet_runtime_line_ok
        self.assertTrue(ok('Microsoft.NETCore.App 8.0.11 [C:\\Program Files\\dotnet]'))
        self.assertTrue(ok('Microsoft.NETCore.App 10.0.1 [C:\\x]'))
        self.assertFalse(ok('Microsoft.NETCore.App 6.0.36 [C:\\x]'))
        self.assertFalse(ok('Microsoft.AspNetCore.App 9.0.0 [C:\\x]'))

    def test_ufs2tool_may_roll_forward(self):
        import inspect
        self.assertIn('LatestMajor', inspect.getsource(self.mod.extract_ufs2tool))


if __name__ == '__main__':
    unittest.main()
