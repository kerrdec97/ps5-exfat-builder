"""Regression tests for the "good image deleted" family of bugs.

Issues #72, #73, #74, #76, #79 and #80: on non-English Windows robocopy's
summary is translated, the copied-file count was never parsed, the build was
judged "0 of N files copied" and the finished image was deleted. The queue
renderer then crashed with ``%d format: a real number is required, not str``
(also #77). The summary lines below are copied from those reports.
"""
import inspect
import os
import tempfile
import unittest
from unittest import mock

from tests._support import load_app, decode_embedded

ITALIAN_76 = [
    '-------------------------------------------------------------------------------',
    '',
    '              Totale   Copiato  IgnorateNon corrispondentiNon riuscitaSupplementari',
    'Directory:     19682     19682         1         0         0         0',
    '     File:    223243    223243         0         0         0         0',
    '     Byte:  82.027 g  82.027 g         0         0         0         0',
    '   Durata:   0:35:14   0:08:48                       0:00:00   0:09:08',
]
FRENCH_80 = [
    '------------------------------------------------------------------------------',
    '   R\u00e9p. :     19682     19682         1         0         0         0',
    'Fichiers :    223239    223239         0         0         0         0',
    '  Octets :  82.027 g  82.027 g         0         0         0         0',
]
FRENCH_79 = [
    '-----------',
    'Dossiers: 1343 1343 0 0 0 0',
    'Fichiers: 52655 52655 0 0 0 0',
    'Octets: 195.441 g 195.441 g 0 0 0 0',
]
CHINESE_72 = [
    '------------------------------------------------------------------------------',
    '       \u76ee\u5f55:      1200      1200         0         0         0         0',
    '       \u6587\u4ef6:    156250    156250         0         0         0         0',
    '       \u5b57\u8282:   148.5 g   148.5 g         0         0         0         0',
]
ENGLISH_SMALL = [
    '------------------------------------------------------------------------------',
    '               Total    Copied   Skipped  Mismatch    FAILED    Extras',
    '    Dirs :         3         2         1         0         0         0',
    '   Files :        10         8         0         0         2         0',
    '   Bytes :      2048      2048         0         0         0         0',
    '   Times :   0:00:00   0:00:00                       0:00:00   0:00:00',
]
PER_FILE_NOISE = [
    '\t\t\t   1048576\tM:\\sce_sys\\param.sfo',
    '100%  ',
    '  Started : Thursday 24 September 2026 13:19:12',
    '   Source : V:\\Giochi Ps5\\PPSA01342-app0\\',
    '[ROBO-MODE] summary',
]


def _files_row(lines):
    p = load_app().RobocopySummaryParser()
    row = None
    for line in lines:
        got = p.feed(line)
        if got is not None:
            row = got
    return row


class RobocopySummaryParsing(unittest.TestCase):
    def test_localized_summaries_from_issue_logs(self):
        self.assertEqual(_files_row(ITALIAN_76)[:2], (223243, 223243))
        self.assertEqual(_files_row(FRENCH_80)[:2], (223239, 223239))
        self.assertEqual(_files_row(FRENCH_79)[:2], (52655, 52655))
        self.assertEqual(_files_row(CHINESE_72)[:2], (156250, 156250))

    def test_english_and_failed_files(self):
        row = _files_row(ENGLISH_SMALL)
        self.assertEqual(row, (10, 8, 0, 0, 2, 0))
        app = load_app()
        self.assertEqual(app.RobocopySummaryParser.files_present(row), 8)

    def test_retry_pass_counts_skipped_files_as_present(self):
        # Second robocopy run (after the /J retry): earlier files are
        # "skipped" because they are already there.
        row = (5000, 3, 4997, 0, 0, 0)
        self.assertEqual(load_app().RobocopySummaryParser.files_present(row), 5000)

    def test_ignores_per_file_and_header_lines(self):
        self.assertIsNone(_files_row(PER_FILE_NOISE))

    def test_copy_stats_line(self):
        app = load_app()
        got = app.parse_copy_stats_line(
            '[COPY-STATS] dest_files=223243 dest_bytes=88074000000 '
            'source_files=223243 source_bytes=88074000000 robocopy_exit=1')
        self.assertEqual(got['dest_files'], 223243)
        self.assertEqual(got['robocopy_exit'], 1)
        self.assertIsNone(app.parse_copy_stats_line('[Info] Post-copy check passed.'))


class _FakeBuild:
    """Just enough of ExFATBuilder for _build_output_is_empty."""

    def __init__(self, **attrs):
        self.logged = []
        self.sanity = (True, 'header ok')
        self.__dict__.update(attrs)

    def _log(self, text):
        self.logged.append(text)

    def _verify_image_file_only(self, _path):
        return self.sanity


class OutputRejectionPolicy(unittest.TestCase):
    def setUp(self):
        self.check = load_app().ExFATBuilder._build_output_is_empty

    def test_unparsed_localized_summary_does_not_reject(self):
        # Exactly the #72-#80 situation: count never parsed (stays 0).
        fake = _FakeBuild(_robocopy_file_count=0, _source_file_count=223243,
                          _robocopy_count_known=False, _copy_stats=None,
                          _build_log_tail=['Fichiers :  223239  223239 0 0 0 0'])
        empty, why = self.check(fake, 'x.exfat')
        self.assertFalse(empty, why)

    def test_copy_stats_with_files_on_volume_is_accepted(self):
        fake = _FakeBuild(_source_file_count=95397, _copy_stats={
            'dest_files': 95397, 'source_files': 95397, 'dest_bytes': 1,
            'source_bytes': 1, 'robocopy_exit': 1})
        self.assertFalse(self.check(fake, 'x.exfat')[0])

    def test_genuinely_empty_volume_is_rejected(self):
        fake = _FakeBuild(_source_file_count=245, _copy_stats={
            'dest_files': 0, 'source_files': 245, 'dest_bytes': 0,
            'source_bytes': 8_000_000_000, 'robocopy_exit': 8})
        empty, why = self.check(fake, 'x.exfat')
        self.assertTrue(empty)
        self.assertIn('0 of 245', why)

    def test_known_zero_count_is_rejected(self):
        fake = _FakeBuild(_robocopy_file_count=0, _source_file_count=12,
                          _robocopy_count_known=True, _copy_stats=None)
        self.assertTrue(self.check(fake, 'x.exfat')[0])

    def test_error_87_text_alone_does_not_reject(self):
        fake = _FakeBuild(_source_file_count=10, _copy_stats={
            'dest_files': 10, 'source_files': 10, 'dest_bytes': 1,
            'source_bytes': 1, 'robocopy_exit': 1},
            _build_log_tail=['ERROR 87 (0x00000057) Copying File a.bin'])
        self.assertFalse(self.check(fake, 'x.exfat')[0])

    def test_corrupt_container_is_still_rejected(self):
        fake = _FakeBuild(_source_file_count=10, _copy_stats=None)
        fake.sanity = (False, 'no exFAT boot sector')
        self.assertTrue(self.check(fake, 'x.exfat')[0])

    def test_item_done_never_deletes_the_image(self):
        src = inspect.getsource(load_app().ExFATBuilder._item_done)
        self.assertNotIn('os.remove(out_path)', src)
        self.assertNotIn('os.unlink(out_path)', src)


class SizeOverrideCoercion(unittest.TestCase):
    def test_values(self):
        c = load_app().coerce_size_gb
        self.assertIsNone(c('auto'))       # Build tab sentinel (#77 crash)
        self.assertIsNone(c(None))
        self.assertIsNone(c(''))
        self.assertIsNone(c('abc'))
        self.assertIsNone(c(0))
        self.assertIsNone(c(-5))
        self.assertIsNone(c(True))
        self.assertEqual(c('120'), 120.0)
        self.assertEqual(c(120.5), 120.5)
        self.assertEqual('%d GB' % c('88'), '88 GB')


class EmbeddedBuildScript(unittest.TestCase):
    def setUp(self):
        self.ps1 = decode_embedded('_PS1_B64')

    def test_safety_features_present(self):
        for needle in ('function Get-UsedDriveLetters', 'HKCU:\\Network',
                       'function Get-TreeFileStats', '[COPY-STATS]',
                       "Where-Object { $_ -ne '/J' }",
                       "'OSFMount attach'", 'TimedOut'):
            self.assertIn(needle, self.ps1, needle)

    def test_retry_message_cannot_trip_error_87_heuristics(self):
        tail = self.ps1.split('Retrying once without /J', 1)[1][:120]
        self.assertNotIn('ERROR 87', tail.upper())

    def test_batch_wrapper_decodes(self):
        bat = decode_embedded('_BAT_B64')
        self.assertIn('New-OsfExfatImage.ps1', bat)


class VerificationHelpers(unittest.TestCase):
    def setUp(self):
        self.app = load_app()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = os.path.join(self.tmp.name, 'src')
        os.makedirs(os.path.join(self.src, 'sce_sys'))
        os.makedirs(os.path.join(self.src, 'decrypted'))
        self._write('eboot.bin', 100)
        self._write(os.path.join('sce_sys', 'param.sfo'), 10)
        self._write('data.pak', 5000)
        self._write('nested.exfat', 7)
        self._write(os.path.join('decrypted', 'skip.bin'), 9)

    def _write(self, rel, size):
        with open(os.path.join(self.src, rel), 'wb') as f:
            f.write(b'\0' * size)

    def test_walk_applies_build_exclusions(self):
        count, total, sizes, eboot = self.app.verify_walk_tree(self.src)
        self.assertEqual(count, 3)
        self.assertEqual(total, 5110)
        self.assertTrue(eboot)
        self.assertNotIn('nested.exfat', sizes)

    def test_hidden_files_skipped_only_when_requested(self):
        hidden = os.path.join(self.src, 'data.pak')
        with mock.patch.object(self.app, '_is_hidden_file',
                               side_effect=lambda p: p == hidden):
            self.assertEqual(self.app.verify_walk_tree(self.src)[0], 3)
            self.assertEqual(self.app.verify_walk_tree(self.src, skip_hidden=True)[0], 2)

    def test_missing_files_are_named(self):
        src = {'data.pak': 5000, 'eboot.bin': 100, 'sce_sys\\param.sfo': 10}
        img = {'eboot.bin': 100, 'sce_sys\\param.sfo': 4}
        lines = self.app.describe_missing_files(src, img)
        self.assertTrue(lines[0].startswith('data.pak (missing'))
        self.assertIn('param.sfo', lines[1])


if __name__ == '__main__':
    unittest.main()
