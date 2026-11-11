"""Writes the golden files of the FPKG tests (tests/engines/fpkg/tst_fpkg_rules.cpp).

The byte format of the project and JSON files is defined as "what Python's
xml.etree.ElementTree / json modules write", so the goldens are produced by
those modules, independently of the C++ writer. The expected *content* (which
entries, in which order, with which attributes) is written out by hand below
from the rules in native/docs/FPKG_PARITY.md section 4.5/4.6 for the synthetic
dump the C++ test creates (makeGoldenDump()).

Also writes flat_path_hash.txt: test vectors of the PS5 flat-path-table hash
from a second, independent port of the algorithm described in LibProsperoPkg
(ProsperoPs5FlatPathTable.HashBytes, GPL-3.0-or-later, SvenGDK).

Run: python3 make_goldens.py   (from this folder; Python 3.9+)
"""
import json
import os
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
CID = 'UP0000-PPSA01234_00-GOLDENTEST000000'
STEM = CID + '-A0100-V0100'
OUT = 'C:\\out'
DUMP = 'C:\\dump'
ASSETS = OUT + '\\.gp5-assets\\' + STEM
PASSCODE = '0' * 32
DESCRIPTION = 'All game data is installed before the game starts.'

# Walk order of the synthetic dump's mapped files (casefold order per folder).
GAME_FILES = ['Data/1.bin', 'Data/_x.bin', 'Data/a&b.bin', 'Data/A.pak', 'Data/b.pak', 'dlc_emu.ini',
              'eboot.bin', 'sce_module/libc.prx', 'sce_sys/icon0.png', 'sce_sys/keystone', 'sce_sys/param.json']
EMPTY_DIRS = ['addcont0', 'Data/Empty', 'Data/JunkOnly']


def win(rel):
    return rel.replace('/', '\\')


def src_of(rel):
    if rel == 'sce_sys/param.json':
        return ASSETS + '\\sce_sys\\param.json'
    return DUMP + '\\' + win(rel)


def write_bytes(name, data):
    with open(os.path.join(HERE, name), 'wb') as f:
        f.write(data)


def gp5_bytes(chunk_info, files):
    root = ET.Element('psproject', {'fmt': 'gp5'})
    vol = ET.SubElement(root, 'volume')
    ET.SubElement(vol, 'volume_type').text = 'prospero_app'
    ET.SubElement(vol, 'package', {'passcode': PASSCODE, 'content_id': CID})
    ci = ET.SubElement(vol, 'chunk_info', chunk_info['attrs'])
    chunks = ET.SubElement(ci, 'chunks', chunk_info['chunks_attrs'])
    for c in chunk_info['chunks']:
        ET.SubElement(chunks, 'chunk', c)
    scs = ET.SubElement(ci, 'scenarios', chunk_info['scenarios_attrs'])
    for attrs, text in chunk_info['scenarios']:
        ET.SubElement(scs, 'scenario', attrs).text = text
    fe = ET.SubElement(root, 'files')
    for attrs in files:
        ET.SubElement(fe, 'file', attrs)
    for d in EMPTY_DIRS:
        ET.SubElement(fe, 'empty_dir', {'dst_path': d})
    ET.indent(root, space='  ')
    text = "<?xml version='1.0' encoding='utf-8'?>\n" + ET.tostring(root, encoding='unicode')
    return text.replace('\n', '\r\n').encode('utf-8')   # no line break after the last line


def file_attrs(dst, src, chunk=None, disable=False):
    a = {'dst_path': dst, 'src_path': src}
    if chunk is not None:
        a['chunk'] = str(chunk)
    if disable:
        a['pfs_compression'] = 'disable'
    return a


def json_bytes(obj):
    return (json.dumps(obj, indent=2, ensure_ascii=False) + '\n').replace('\n', '\r\n').encode('utf-8')


def default_scenario(lang):
    return {'chunkDefaultLanguage': lang, 'chunkSupportedLanguages': [lang], 'scenarioCount': 1,
            'scenarioDefaultId': 0, 'scenarioDefaultLanguage': lang,
            'scenarios': [{'id': 0, 'type': 'playmode', lang: {'title': 'Scenario #0', 'description': DESCRIPTION}}]}


SOURCE_SCENARIO = ('{"chunkDefaultLanguage":"en-US","chunkSupportedLanguages":["en-US","fr-FR"],"scenarioCount":2,'
                   '"scenarioDefaultId":0,"scenarioDefaultLanguage":"en-US","scenarios":[{"id":0,"type":"playmode",'
                   '"en-US":{"title":"Main game","description":"Everything"}},{"id":1,"type":"mp","en-US":{"title":'
                   '"Multiplayer","description":"Online"},"fr-FR":{"title":"Multi","description":"En ligne"}}]}')

SOURCE_PARAM = ('{"contentId":"' + CID + '","contentVersion":"01.000.000","masterVersion":"01.000.000",'
                '"applicationCategoryType":0,"applicationDrmType":"free","attribute3":8388736,'
                '"versionFileUri":"http://example.invalid/version.xml","sdkVersion":"0x0114000000080001",'
                '"requiredSystemSoftwareVersion":"0x0200000000000000","kernel":{"addcontMountLevel":2,'
                '"appSizeInGib":5},"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":'
                '"G\\u00f6lden \\u2603"}}}')


def layout_b():
    info = {
        'attrs': {'chunk_count': '3', 'scenario_count': '2'},
        'chunks_attrs': {'supported_languages': 'en-US fr-FR', 'default_language': 'en-US'},
        # Chunk 0 keeps the game data for every language; language i (from 1) goes to chunk min(i, N - 1).
        'chunks': [{'id': '0', 'label': 'Chunk #0', 'layer_no': '0', 'languages': 'en-US fr-FR'},
                   {'id': '1', 'label': 'Chunk #1', 'layer_no': '0', 'languages': 'en-US'},
                   {'id': '2', 'label': 'Chunk #2', 'layer_no': '0', 'languages': 'fr-FR'}],
        'scenarios_attrs': {'default_id': '0'},
        'scenarios': [({'id': '0', 'type': 'playmode', 'initial_chunk_count': '3', 'label': 'Main game'}, '0-2'),
                      ({'id': '1', 'type': 'mp', 'initial_chunk_count': '3', 'label': 'Multiplayer'}, '0-2')],
    }
    files = [file_attrs('sce_sys/playgo-scenario.json', OUT + '\\' + STEM + '.playgo-scenario.json', 0),
             file_attrs('playgo-languages/01-en-US.bin', ASSETS + '\\playgo-languages\\01-en-US.bin', 1, True),
             file_attrs('playgo-languages/02-fr-FR.bin', ASSETS + '\\playgo-languages\\02-fr-FR.bin', 2, True)]
    files += [file_attrs(r, src_of(r), 0) for r in GAME_FILES]
    return gp5_bytes(info, files)


def layout_a():
    info = {
        'attrs': {'chunk_count': '1', 'scenario_count': '1'},
        'chunks_attrs': {},
        'chunks': [{'id': '0', 'label': 'Chunk #0'}],
        'scenarios_attrs': {'default_id': '0'},
        'scenarios': [({'id': '0', 'type': 'playmode', 'initial_chunk_count': '1', 'label': 'Scenario #0'}, '0')],
    }
    files = [file_attrs('sce_sys/playgo-scenario.json', OUT + '\\' + STEM + '.playgo-scenario.json')]
    files += [file_attrs(r, src_of(r)) for r in GAME_FILES]
    return gp5_bytes(info, files)


def layout_c():
    info = {
        'attrs': {'chunk_count': '3', 'scenario_count': '2'},
        'chunks_attrs': {'supported_languages': 'en-US fr-FR de-DE', 'default_language': 'en-US'},
        # The dump's chunk names and languages; the scenario types of playgo-scenario.json with the
        # names of the dump's table; every scenario lists every chunk and every chunk is initial.
        'chunks': [{'id': '0', 'label': 'Base', 'layer_no': '0', 'languages': 'en-US fr-FR de-DE'},
                   {'id': '1', 'label': 'French voice', 'layer_no': '0', 'languages': 'fr-FR'},
                   {'id': '2', 'label': 'Extra', 'layer_no': '0', 'languages': 'en-US fr-FR de-DE'}],
        'scenarios_attrs': {'default_id': '0'},
        'scenarios': [({'id': '0', 'type': 'playmode', 'initial_chunk_count': '3', 'label': 'Main'}, '0-2'),
                      ({'id': '1', 'type': 'mp', 'initial_chunk_count': '3', 'label': 'Second'}, '0-2')],
    }
    # Every file names its chunk; a file the dump's map does not name goes to chunk 0. Chunk 2 only
    # holds an empty file, so it gets a placeholder.
    chunk_of = {'Data/A.pak': 1, 'Data/_x.bin': 2}
    files = [file_attrs('sce_sys/playgo-scenario.json', OUT + '\\' + STEM + '.playgo-scenario.json', 0),
             file_attrs('playgo-chunks/002.bin', ASSETS + '\\playgo-chunks\\002.bin', 2, True)]
    files += [file_attrs(r, src_of(r), chunk_of.get(r, 0)) for r in GAME_FILES]
    return gp5_bytes(info, files)


def scenario_c():
    # The dump's own scenario file with the languages of the dump's chunks.
    s = json.loads(SOURCE_SCENARIO)
    s['chunkSupportedLanguages'] = ['en-US', 'fr-FR', 'de-DE']
    return s


def normalised_param():
    p = json.loads(SOURCE_PARAM)            # dicts keep source order
    p['applicationDrmType'] = 'standard'
    p['versionFileUri'] = ''
    p['requiredSystemSoftwareVersion'] = '0x%016x' % (int(p['sdkVersion'], 16) & 0xFFFF000000000000)
    del p['kernel']['appSizeInGib']
    p['attributePub'] = 0
    return p


# ---- PS5 flat-path-table hash (second port, for test vectors) ---------------
M = (1 << 64) - 1
SEED0, SEED1, ROUND = 0x92ca8aab26a24f51, 0x09bbb761a41bc44d, 0x8000000080008081


def rotl(x, n):
    return ((x << n) | (x >> (64 - n))) & M


def rotr(x, n):
    return ((x >> n) | (x << (64 - n))) & M


def flat_hash(path):
    if path.startswith('/'):
        path = path[1:]
    data = path.upper().encode('ascii')
    s0, s1, s2 = SEED0, rotl(SEED0, 11), rotl(SEED0, 23)
    tail = 0
    if data:
        nwords = (len(data) - 1) >> 3
        a0, a1, a2 = s0, s1, s2
        off = 0
        for _ in range(nwords):
            w = int.from_bytes(data[off:off + 8], 'little')
            off += 8
            a0 ^= w
            t18 = rotr(rotl(a2 ^ a1, 5) ^ a0, 11)
            t12 = rotl(rotl(a2 ^ a0, 17) ^ a1, 11)
            a2 = rotr(rotl(a1 ^ a0, 1) ^ a2, 5)
            a0 = ((~t12 & M) & a2) ^ t18 ^ ROUND
            a1 = ((~a2 & M) & t18) ^ t12
            a2 = ((~t18 & M) & t12) ^ a2
        s0, s1, s2 = a0, a1, a2
        tail = int.from_bytes(data[off:off + ((len(data) - 1) & 7) + 1], 'little')
    u16, u11, u6 = s1, s2, tail ^ s0 ^ SEED1
    u17 = rotl(u11 ^ u16, 5) ^ u6
    u18 = rotl(u11 ^ u6, 17) ^ u16
    u11 = rotl(u16 ^ u6, 1) ^ u11
    return ((~rotl(u18, 11) & M) & rotr(u11, 5)) ^ rotr(u17, 11) ^ ROUND


def main():
    write_bytes('gp5_layout_a.gp5', layout_a())
    write_bytes('gp5_layout_b.gp5', layout_b())
    write_bytes('gp5_layout_c.gp5', layout_c())
    write_bytes('scenario_default_en-US.json', json_bytes(default_scenario('en-US')))
    write_bytes('scenario_layout_b.json', json_bytes(json.loads(SOURCE_SCENARIO)))
    write_bytes('scenario_layout_c.json', json_bytes(scenario_c()))
    write_bytes('param_normalised.json', json_bytes(normalised_param()))
    write_bytes('source_param.json', SOURCE_PARAM.encode('utf-8'))
    write_bytes('source_scenario.json', SOURCE_SCENARIO.encode('utf-8'))
    paths = ['', '/sce_sys/keystone', 'eboot.bin', 'Data/A.pak', '/Data/_x.bin',
             'sce_module/libc.prx', 'a/very/long/path/with/many/components/file.bin']
    write_bytes('flat_path_hash.txt',
                ''.join('%s\t%016x\n' % (p, flat_hash(p)) for p in paths).encode('ascii'))


if __name__ == '__main__':
    main()
