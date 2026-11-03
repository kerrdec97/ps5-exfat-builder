"""The embedded PowerShell build script must parse (Windows only)."""
import base64
import os
import subprocess
import tempfile
import unittest

from tests._support import load_app


@unittest.skipUnless(os.name == 'nt', 'needs Windows PowerShell')
class EmbeddedScripts(unittest.TestCase):
    def test_ps1_parses_without_errors(self):
        mod = load_app()
        d = tempfile.mkdtemp(prefix='ps1_')
        p = os.path.join(d, 'make_image.ps1')
        with open(p, 'wb') as f:
            f.write(base64.b64decode(mod._PS1_B64))
        cmd = ('$e=$null; $t=$null; '
               '[void][System.Management.Automation.Language.Parser]::ParseFile('
               "'%s', [ref]$t, [ref]$e); "
               'if ($e.Count) { $e | ForEach-Object { $_.ToString() }; exit 1 } '
               'else { "ok" }' % p.replace("'", "''"))
        r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive',
                            '-Command', cmd], capture_output=True, text=True,
                           timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('ok', r.stdout)
