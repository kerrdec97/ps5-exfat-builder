# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_submodules
from PyInstaller.utils.hooks import collect_all

datas = []
binaries = []
hiddenimports = ['PIL._tkinter_finder', 'mkpfs', 'mkpfs.cli', 'mkpfs.pfs', 'mkpfs.utils', 'mkpfs.consts', 'mkpfs.logging', 'mkpfs.pbar', 'cryptography', 'cryptography.hazmat.primitives.ciphers', 'cryptography.hazmat.primitives.ciphers.algorithms', 'cryptography.hazmat.primitives.ciphers.modes', 'cryptography.hazmat.backends', 'cryptography.hazmat.backends.openssl', 'cryptography.hazmat.backends.openssl.backend']
hiddenimports += collect_submodules('ui')
hiddenimports += collect_submodules('ui.shared')
tmp_ret = collect_all('mkpfs')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('cryptography')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('tkinterdnd2')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]


a = Analysis(
    ['exfat_builder.py'],
    pathex=['.'],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='exFAT Image Builder',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['controller.ico'],
)
