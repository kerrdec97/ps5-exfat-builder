"""Game metadata parsing, output naming and version comparison.

These helpers decide what every image is called and whether the updater
offers a new release, so they are pinned down here with synthetic inputs.
"""
import json
import os
import struct
import tempfile
import unittest

from tests._support import load_app


def _make_sfo(entries):
    """Build a minimal PS4/PS5 param.sfo containing UTF-8 string and int
    entries. ``entries`` is a list of (key, value) pairs."""
    keys = b''
    data = b''
    index = b''
    for key, value in entries:
        key_off = len(keys)
        keys += key.encode('utf-8') + b'\x00'
        if isinstance(value, int):
            raw, fmt = struct.pack('<I', value), 0x0404
        else:
            raw, fmt = value.encode('utf-8') + b'\x00', 0x0204
        data_off = len(data)
        data += raw
        index += struct.pack('<HHIII', key_off, fmt, len(raw), len(raw), data_off)
    while len(keys) % 4:
        keys += b'\x00'
    key_table = 20 + len(index)
    data_table = key_table + len(keys)
    header = struct.pack('<IIIII', 0x46535000, 0x0101, key_table, data_table,
                         len(entries))
    return header + index + keys + data


class SfoParsing(unittest.TestCase):
    def setUp(self):
        self.app = load_app()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def _write(self, name, blob):
        path = os.path.join(self.tmp.name, name)
        with open(path, 'wb') as f:
            f.write(blob)
        return path

    def test_reads_strings_and_integers(self):
        path = self._write('param.sfo', _make_sfo([
            ('TITLE', 'Ghost of Yōtei'),
            ('TITLE_ID', 'PPSA26344'),
            ('APP_VER', '01.512'),
            ('PARENTAL_LEVEL', 5),
        ]))
        sfo = self.app.parse_sfo(path)
        self.assertEqual(sfo['TITLE'], 'Ghost of Yōtei')
        self.assertEqual(sfo['TITLE_ID'], 'PPSA26344')
        self.assertEqual(sfo['APP_VER'], '01.512')
        self.assertEqual(sfo['PARENTAL_LEVEL'], 5)

    def test_rejects_garbage_without_raising(self):
        self.assertEqual(self.app.parse_sfo(self._write('a.sfo', b'not an sfo file')), {})
        self.assertEqual(self.app.parse_sfo(self._write('b.sfo', b'')), {})
        self.assertEqual(self.app.parse_sfo(os.path.join(self.tmp.name, 'missing.sfo')), {})

    def test_param_json_prefers_default_language(self):
        path = os.path.join(self.tmp.name, 'param.json')
        with open(path, 'w', encoding='utf-8') as f:
            json.dump({'titleId': 'PPSA01342', 'contentVersion': 'x',
                       'masterVersion': '01.000.000',
                       'localizedParameters': {
                           'defaultLanguage': 'fr-FR',
                           'fr-FR': {'titleName': 'Démon'},
                           'en-US': {'titleName': "Demon's Souls"}}}, f)
        title, tid, ver = self.app.parse_param_json(path)
        self.assertEqual((title, tid, ver), ('Démon', 'PPSA01342', '01.000.000'))

    def test_param_json_failure_returns_empty_triple(self):
        path = self._write('param.json', b'{broken')
        self.assertEqual(self.app.parse_param_json(path), ('', '', ''))


class OutputNaming(unittest.TestCase):
    def setUp(self):
        self.app = load_app()

    def test_windows_reserved_and_shell_characters_removed(self):
        cleaned = self.app.sanitize_filename('Ratchet & Clank: Rift <Apart>?|*"')
        for ch in '\\/:*?"<>|&':
            self.assertNotIn(ch, cleaned)
        self.assertTrue(cleaned.strip())

    def test_name_contains_id_title_and_version(self):
        name = self.app.build_exfat_name("Demon's Souls", 'PPSA01342', '01.000.000')
        self.assertTrue(name.endswith('.exfat'))
        self.assertIn('PPSA01342', name)
        self.assertIn('01.000.000', name)

    def test_name_falls_back_when_nothing_known(self):
        self.assertEqual(self.app.build_exfat_name('', '', ''), 'game.exfat')

    def test_title_equal_to_id_is_not_repeated(self):
        name = self.app.build_exfat_name('PPSA00001', 'PPSA00001', '')
        self.assertEqual(name.count('PPSA00001'), 1)


class VersionComparison(unittest.TestCase):
    def setUp(self):
        self.parse = load_app().ExFATBuilder._parse_version_tuple

    def test_formats(self):
        self.assertEqual(self.parse('v4.0.2'), (4, 0, 2))
        self.assertEqual(self.parse('V2.7'), (2, 7, 0))
        self.assertEqual(self.parse('4.1.0-rc1'), (4, 1, 0))
        self.assertEqual(self.parse(''), (0, 0, 0))
        self.assertEqual(self.parse('garbage'), (0, 0, 0))

    def test_farewell_is_newer_than_last_release(self):
        app = load_app()
        self.assertGreater(self.parse(app.APP_VERSION), self.parse('v4.0.2'))


if __name__ == '__main__':
    unittest.main()
