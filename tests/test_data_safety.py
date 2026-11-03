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
