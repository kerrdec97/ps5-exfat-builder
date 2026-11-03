"""Real UFS2Tool run (Windows only): build a tiny .ffpkg the way the app
does, writing to '<name>.part' and renaming it on success.

Needs the .NET runtime UFS2Tool targets; skipped when it is missing or
when not on Windows. The image is checked for the UFS2 superblock magic.
"""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

from tests._support import load_app

UFS2_MAGIC = 0x19540119
SBLOCK_UFS2 = 65536
FS_MAGIC_OFFSET = 1372


@unittest.skipUnless(os.name == 'nt', 'UFS2Tool runs on Windows only')
class Ufs2ToolBuild(unittest.TestCase):
    def setUp(self):
        self.mod = load_app()
        self.tmp = tempfile.mkdtemp(prefix='ufs2_test_')
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.exe = self.mod.extract_ufs2tool(self.tmp)

    def _dump(self):
        src = os.path.join(self.tmp, 'PPSA00000-app')
        os.makedirs(os.path.join(src, 'sce_sys'))
        with open(os.path.join(src, 'eboot.bin'), 'wb') as f:
            f.write(b'\x7fELF' + os.urandom(200000))
        with open(os.path.join(src, 'sce_sys', 'param.json'), 'w') as f:
            f.write('{"titleId": "PPSA00000"}')
        return src

    def test_newfs_into_part_then_rename(self):
        src = self._dump()
        out = os.path.join(self.tmp, 'out', 'Test Game.ffpkg')
        os.makedirs(os.path.dirname(out))
        part = self.mod.ffpkg_part_path(out)
        cmd = [self.exe, 'newfs', '-O', '2', '-b', '65536', '-f', '65536',
               '-m', '0', '-S', '512', '-i', '262144', '-D', src, part]
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding='utf-8', errors='replace',
                           cwd=os.path.dirname(self.exe), timeout=300)
        log = (r.stdout or '') + (r.stderr or '')
        hint = self.mod.dotnet_runtime_hint(r.returncode)
        if hint:
            self.skipTest('.NET runtime for UFS2Tool not installed: ' + hint)
        self.assertEqual(r.returncode, 0, log[-3000:])
        self.assertTrue(os.path.isfile(part), log[-3000:])
        self.assertFalse(os.path.exists(out))

        rc, msg = self.mod.finalize_ffpkg_part(0, out)
        self.assertEqual(rc, 0, msg)
        self.assertTrue(os.path.isfile(out))
        self.assertFalse(os.path.exists(part))
        with open(out, 'rb') as f:
            f.seek(SBLOCK_UFS2 + FS_MAGIC_OFFSET)
            magic = struct.unpack('<I', f.read(4))[0]
        self.assertEqual(magic, UFS2_MAGIC)
        ok, why = self.mod.ffpkg_safe_to_delete_source(src, out)
        self.assertTrue(ok, why)


if __name__ == '__main__':
    unittest.main()
