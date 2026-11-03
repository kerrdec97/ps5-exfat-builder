"""Structural checks of a built PS5 package.

These read only the outer header and do not need any keys:
  * at least 4096 bytes, magic 7F 'FIH' at offset 0;
  * byte 5 (signed flag) is 0: a debug package is not signed;
  * the outer superblock offset (u64 LE at 0x20) lies inside the file;
  * the superblock mode (u16 LE at +0x1C) is 13;
  * a plaintext debug package carries 'PPRPLAIN-NOAUTH!' at +0x370.
A package without the marker was made by a Publishing Tools build that
does not produce installable debug packages.
"""
import hashlib
import os
import struct

FIH_MAGIC = b'\x7fFIH'
PLAIN_MARKER = b'PPRPLAIN-NOAUTH!'
SB_MODE = 13


class Check:
    def __init__(self):
        self.ok = False
        self.plaintext = False
        self.problems = []
        self.info = {}

    def __repr__(self):
        return '<Check ok=%s plaintext=%s %s>' % (self.ok, self.plaintext,
                                                    self.problems)


def check_header(path):
    c = Check()
    try:
        size = os.path.getsize(path)
        with open(path, 'rb') as f:
            head = f.read(0x60)
            if size < 4096:
                c.problems.append('file is only %d bytes' % size)
                return c
            if head[:4] != FIH_MAGIC:
                c.problems.append('not a PS5 package (no FIH header)')
                return c
            if head[5] != 0:
                c.problems.append('package is signed (byte 5 = %d); this is '
                                  'not a debug FPKG' % head[5])
            sb = struct.unpack_from('<Q', head, 0x20)[0]
            c.info['superblock_offset'] = sb
            if sb + 896 > size:
                c.problems.append('superblock offset 0x%x is outside the file'
                                  % sb)
                return c
            f.seek(sb)
            block = f.read(0x380)
    except OSError as e:
        c.problems.append('cannot read: %s' % e)
        return c
    mode = struct.unpack_from('<H', block, 0x1C)[0]
    c.info['mode'] = mode
    if mode != SB_MODE:
        c.problems.append('unexpected superblock mode %d (expected %d)'
                          % (mode, SB_MODE))
    c.plaintext = block[0x370:0x380] == PLAIN_MARKER
    if not c.plaintext:
        c.problems.append('the plaintext debug marker is missing: these '
                          'Publishing Tools do not produce an installable '
                          'debug FPKG')
    c.ok = not c.problems
    return c


def sha256(path, cancel_check=None):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            if cancel_check and cancel_check():
                return None
            b = f.read(4 * 1024 * 1024)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def img_verify_argv(exe, pkg, passcode='0' * 32, integrity=True):
    return [exe, 'img_verify', '--passcode', passcode, '--format_check', 'on',
            '--integrity_check', 'on' if integrity else 'off',
            '--no_progress_bar', pkg]


def img_extract_argv(exe, pkg, dest, passcode='0' * 32):
    return [exe, 'img_extract', '--passcode', passcode, '--no_progress_bar',
            pkg, dest]
