"""Stand-in for prospero-pub-cmd.exe used by the FPKG tests.

img_create: checks the GP5 lists existing source files, prints a progress
bar like the real tool and writes a package with a debug-FPKG header.
FAKE_PUBCMD_MODE: ok | stock (no plaintext marker) | error ([Error] line,
exit 0) | crash (exit 3) | nooutput | slow (for cancel tests).
img_verify: exit 0, or 1 with FAKE_PUBCMD_VERIFY=fail.
"""
import os
import struct
import sys
import time
import xml.etree.ElementTree as ET


def write_pkg(path, marker=True):
    sb = 0x1000
    data = bytearray(0x2000)
    data[0:4] = b'\x7fFIH'
    struct.pack_into('<Q', data, 0x20, sb)
    struct.pack_into('<H', data, sb + 0x1C, 13)
    if marker:
        data[sb + 0x370:sb + 0x380] = b'PPRPLAIN-NOAUTH!'
    with open(path, 'wb') as f:
        f.write(data)


def main(argv):
    mode = os.environ.get('FAKE_PUBCMD_MODE', 'ok')
    cmd = argv[0]
    if cmd == 'img_create':
        gp5, out = argv[-2], argv[-1]
        root = ET.parse(gp5).getroot()
        missing = [f.get('src_path') for f in root.iter('file')
                   if not os.path.isfile(f.get('src_path'))]
        if missing:
            print('[Error] File not found (%s)' % missing[0])
            return 1
        print('[Info] Creating an image...')
        print('|____|____|____|____|____|____|____|____|____|____|')
        sys.stdout.flush()
        for i in range(51):
            sys.stdout.write('=')
            sys.stdout.flush()
            time.sleep(0.2 if mode == 'slow' else 0.002)
            if i == 10 and mode in ('ok', 'stock', 'error', 'slow'):
                with open(out, 'wb') as f:
                    f.write(b'partial')
        print()
        if mode == 'crash':
            return 3
        if mode == 'nooutput':
            if os.path.exists(out):
                os.remove(out)
            return 0
        write_pkg(out, marker=(mode != 'stock'))
        if '--ref_pkg_path' in argv:
            write_pkg(out + '.remastered.pkg', marker=(mode != 'stock'))
        if mode == 'error':
            print('[Error] Format of the png file is not valid (sce_sys/pic2.png)')
        print('[Warn] The version of the SDK toolchain is invalid or missing')
        print('[Info] Process finished.')
        return 0
    if cmd == 'img_verify':
        if os.environ.get('FAKE_PUBCMD_VERIFY') == 'fail':
            print('[Error] Integrity check failed')
            return 1
        print('[Info] Verification passed')
        return 0
    if cmd == 'img_extract':
        pkg, dest = argv[-2], argv[-1]
        if os.environ.get('FAKE_PUBCMD_EXTRACT') == 'fail':
            print('[Error] Cannot open package')
            return 1
        os.makedirs(os.path.join(dest, 'sce_sys'), exist_ok=True)
        with open(os.path.join(dest, 'eboot.bin'), 'wb') as f:
            f.write(b'\x7fELF')
        with open(os.path.join(dest, 'sce_sys', 'param.json'), 'w') as f:
            f.write('{}')
        print('[Info] Extraction finished')
        return 0
    print('[Error] unknown command %s' % cmd)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
