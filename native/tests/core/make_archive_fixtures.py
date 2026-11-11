"""Makes the 7z fixtures of tests/core/tst_archive_source.cpp.

Run by hand, never by the tests:

    python -I make_archive_fixtures.py "C:\\Program Files\\7-Zip\\7z.exe" <out dir>

The fixtures in tests/data/archives/ were made with 7-Zip 26.05 (x64) on
Windows. They hold a small made-up game (no real game data); the test knows
the contents by the same rules as this script. libarchive's own writer, which
the tests use for everything else, always writes one solid block and never
encrypts, so the cases below need the real program:

    game-lzma2-nonsolid.7z   every file in its own block
    game-lzma-solid.7z       LZMA (not LZMA2), one block
    game-ppmd.7z             PPMd
    game-deflate.7z          Deflate
    game-bcj-lzma2.7z        x86 filter + LZMA2
    game-delta-lzma2.7z      delta filter + LZMA2
    game-arm64-lzma2.7z      ARM64 filter + LZMA2
    game-plain-header.7z     list of files stored, not packed
    game-in-folder.7z        the game one folder down
    game-bzip2.7z            BZip2, which the app does not read
    game-encrypted.7z        file data encrypted (password "fixture")
    game-encrypted-header.7z list of files encrypted too
"""
import os
import shutil
import subprocess
import sys
import tempfile

MTIME = 1717243200  # 2024-06-01 12:00:00 UTC


def game_files():
    return {
        "eboot.bin": bytes((i * 31 + 7) & 0xFF for i in range(3000)),
        "sce_sys/param.json": b'{"titleId":"PPSA01234","titleName":"Archive test"}',
        "data/\u30c7\u30fc\u30bf \u00fc.bin": bytes((i * 7) & 0xFF for i in range(257)),
        "data/empty.bin": b"",
    }


def make_tree(root):
    for rel, data in game_files().items():
        path = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        os.utime(path, (MTIME, MTIME))
    os.makedirs(os.path.join(root, "data", "empty folder"), exist_ok=True)


def main():
    seven_zip, out = sys.argv[1], os.path.abspath(sys.argv[2])
    os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="fw-archive-fixtures-")
    try:
        game = os.path.join(work, "PPSA01234-app0")
        make_tree(game)

        def pack(name, switches, cwd=game, what="*"):
            target = os.path.join(out, name)
            if os.path.exists(target):
                os.remove(target)
            subprocess.run([seven_zip, "a", "-t7z", "-bd", "-y", *switches, target, what], cwd=cwd, check=True,
                           stdout=subprocess.DEVNULL)

        pack("game-lzma2-nonsolid.7z", ["-ms=off", "-m0=lzma2"])
        pack("game-lzma-solid.7z", ["-ms=on", "-m0=lzma"])
        pack("game-ppmd.7z", ["-m0=ppmd"])
        pack("game-deflate.7z", ["-m0=deflate"])
        pack("game-bcj-lzma2.7z", ["-m0=bcj", "-m1=lzma2"])
        pack("game-delta-lzma2.7z", ["-m0=delta:4", "-m1=lzma2"])
        pack("game-arm64-lzma2.7z", ["-m0=arm64", "-m1=lzma2"])
        pack("game-plain-header.7z", ["-mhc=off", "-m0=lzma2"])
        pack("game-in-folder.7z", ["-ms=off", "-m0=lzma2"], cwd=work, what="PPSA01234-app0")
        pack("game-bzip2.7z", ["-m0=bzip2"])
        pack("game-encrypted.7z", ["-pfixture", "-m0=lzma2"])
        pack("game-encrypted-header.7z", ["-pfixture", "-mhe=on", "-m0=lzma2"])
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
