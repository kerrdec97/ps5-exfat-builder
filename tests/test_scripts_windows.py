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

    def test_bat_runs_powershell_script_until_osfmount_check(self):
        # No OSFMount on the CI runner: the batch file must start the
        # PowerShell script by its full path, the script must stop at its
        # OSFMount check, and the batch file must report the failure.
        mod = load_app()
        d = tempfile.mkdtemp(prefix='bat_')
        bat, _ico = mod.extract_scripts(d)
        src = os.path.join(d, 'PPSA00000-app')
        os.makedirs(src)
        with open(os.path.join(src, 'eboot.bin'), 'wb') as f:
            f.write(b'\x7fELF')
        image = os.path.join(d, 'out', 'Test.exfat')
        os.makedirs(os.path.dirname(image))
        env = dict(os.environ, EXFAT_SCRIPT_DIR=os.path.dirname(bat))
        r = subprocess.run('cmd.exe /d /s /c ""%s" "%s" "%s""' % (bat, image, src),
                           capture_output=True, timeout=300, env=env,
                           cwd=os.path.dirname(bat))
        out = (r.stdout + r.stderr).decode('oem', errors='replace')
        if 'OSFMount not found' not in out and 'osfmount' not in out.lower():
            self.skipTest('OSFMount appears to be installed on this runner')
        self.assertNotEqual(r.returncode, 0, out[-3000:])
        self.assertIn('PowerShell build failed', out, out[-3000:])
        self.assertFalse(os.path.exists(image))

