"""Which code page do the build's console tools write to a pipe?

The build reads cmd.exe / robocopy / PowerShell output and decodes it as
'mbcs' (the ANSI code page). Console programs usually write the OEM code
page instead, which differs for accented letters on Western systems
(technical audit M10). This probe runs each tool on a non-ASCII folder
name and records which decoding gives the name back; the result is saved
to ci-out/console-encoding.json. It asserts only that the probe ran, so
the evidence can be reviewed before the decoding is changed.
"""
import json
import os
import shutil
import subprocess
import tempfile
import unittest

NAME = 'Ghost of Yōtei éè'


@unittest.skipUnless(os.name == 'nt', 'Windows console code pages')
class ConsoleEncodingProbe(unittest.TestCase):
    def test_probe(self):
        d = tempfile.mkdtemp(prefix='enc_')
        self.addCleanup(shutil.rmtree, d, True)
        src = os.path.join(d, NAME)
        os.makedirs(src)
        with open(os.path.join(src, 'fé.bin'), 'wb') as f:
            f.write(b'x')
        dst = os.path.join(d, 'dst')
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        probes = {
            'cmd_echo': 'cmd.exe /d /s /c "echo %s"' % src,
            'robocopy': ['robocopy', src, dst, '/E', '/L', '/NJH', '/NDL',
                         '/NC', '/NS'],
            'powershell': ['powershell', '-NoProfile', '-Command',
                           "Write-Host '%s'" % src.replace("'", "''")],
        }
        results = {}
        for key, cmd in probes.items():
            raw = subprocess.run(cmd, capture_output=True,
                                 creationflags=flags).stdout
            res = {}
            for enc in ('mbcs', 'oem', 'utf-8'):
                try:
                    txt = raw.decode(enc, errors='replace')
                except LookupError:
                    continue
                # 'é' exists in both the ANSI and OEM code pages of a
                # Western system ('ō' exists in neither and becomes '?').
                res[enc] = ('Y?tei \u00e9\u00e8' in txt or 'f\u00e9.bin' in txt
                            or NAME in txt)
            results[key] = res
        try:
            import ctypes
            results['acp'] = ctypes.windll.kernel32.GetACP()
            results['oemcp'] = ctypes.windll.kernel32.GetOEMCP()
        except Exception:
            pass
        os.makedirs('ci-out', exist_ok=True)
        with open(os.path.join('ci-out', 'console-encoding.json'), 'w') as f:
            json.dump(results, f, indent=2)
        print('CONSOLE-ENCODING', json.dumps(results))
        self.assertIn('cmd_echo', results)
        # The build decodes its console output with the OEM code page; on a
        # system where ANSI and OEM differ, robocopy must come back intact.
        if results.get('acp') != results.get('oemcp'):
            self.assertTrue(results['robocopy'].get('oem'), results)
