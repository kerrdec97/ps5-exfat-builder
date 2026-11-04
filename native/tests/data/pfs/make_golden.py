#!/usr/bin/env python3
"""Regenerate the PFS golden references with the vendored Lazy_MkPFS.

    python3 -I native/tests/data/pfs/make_golden.py <scratch-dir> [--write]

For every case in golden_cases.json this script generates the synthetic source
tree (same generator as tst_pfs_golden.cpp), builds the reference image with
vendor/lazy_mkpfs exactly as the legacy app / Lazy's CLIs would, and prints the
SHA-256. With --write it stores the hashes in golden.json (and small images
under images/). The build timestamp is pinned (Lazy stamps every inode with
int(time.time())) and Lazy's disk mode is used (--no-ram / use_ram_if_possible
=False), see native/docs/engines/PFS.md.

Nothing is installed and the repository is not modified except for golden.json
and images/ with --write. Lazy's own source-folder side effects (it writes
ampr_emu.index into the source) only touch the scratch copies.
"""
from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import sys
import time
import zlib
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
LAZY = os.path.join(REPO, 'vendor', 'lazy_mkpfs')
SPEC = os.path.join(HERE, 'golden_cases.json')
MASK = (1 << 64) - 1
WORDS = [b"alpha ", b"beta ", b"gamma ", b"delta ", b"lorem ", b"ipsum ", b"PS5 ", b"\n"]


class Rng:
    """SplitMix64."""

    def __init__(self, seed: int):
        self.state = seed & MASK

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return z ^ (z >> 31)


def gen_text(rng: Rng, n: int) -> bytes:
    out = []
    total = 0
    while total < n:
        w = WORDS[rng.next() % 8]
        out.append(w)
        total += len(w)
    return b"".join(out)[:n]


def gen_random(rng: Rng, n: int) -> bytes:
    out = bytearray()
    for _ in range((n + 7) // 8):
        out += rng.next().to_bytes(8, 'little')
    return bytes(out[:n])


def gen_content(kind: str, size: int, seed: int) -> bytes:
    rng = Rng(seed)
    if kind == 'text':
        return gen_text(rng, size)
    if kind == 'random':
        return gen_random(rng, size)
    if kind == 'zeros':
        return b"\0" * size
    if kind == 'mixed':
        parts = []
        for c in range((size + 65535) // 65536):
            n = min(65536, size - c * 65536)
            parts.append(gen_text(rng, n) if c % 2 == 0 else gen_random(rng, n))
        return b"".join(parts)
    if kind == 'sparse':
        out = bytearray(size)
        for pos in range(0, size, 4096):
            out[pos] = rng.next() & 0xFF
        return bytes(out)
    raise ValueError(kind)


def expand(tree: list) -> list:
    items = []
    for e in tree:
        if e.get('generate') == 'many':
            for i in range(e['count']):
                items.append({'path': f"{e['dir']}/{e['prefix']}{i:03d}{e['suffix']}", 'kind': e['kind'],
                              'size': e['size'] + (i % 7) * 13, 'seed': e['seed'] + i})
        else:
            items.append(e)
    return items


def make_tree(root: str, tree: list, mtime: int, seed_markers: bool) -> None:
    if os.path.isdir(root):
        shutil.rmtree(root)
    os.makedirs(root)
    for e in expand(tree):
        p = os.path.join(root, *e['path'].rstrip('/').split('/'))
        if e['kind'] == 'dir':
            os.makedirs(p, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(gen_content(e['kind'], e['size'], e['seed']))
        os.utime(p, (mtime, mtime))
    if seed_markers:  # what the legacy PFS tab did before calling mkpfs
        for dirpath, dirnames, filenames in os.walk(root):
            if not dirnames and not filenames:
                m = os.path.join(dirpath, '.pfskeep')
                open(m, 'wb').close()
                os.utime(m, (mtime, mtime))


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def run_cli(module_name: str, argv: list) -> None:
    import importlib
    mod = importlib.import_module(module_name)
    sys.argv = [module_name + '.py'] + argv
    try:
        mod.main()
    except SystemExit as e:
        if e.code not in (None, 0):
            raise RuntimeError(f"{module_name} exited with {e.code}")


def build_case(case: dict, spec: dict, work: str) -> str:
    from lazy_mkpfs.build import build_pfs
    tree = spec['trees'][case['tree']]
    src = os.path.join(work, 'src')
    make_tree(src, tree, spec['file_mtime'], bool(case.get('keep_empty', False)))
    out = os.path.join(work, 'out.img')
    if os.path.exists(out):
        os.remove(out)
    bs = case.get('block_size', 65536)
    level = str(case.get('level', 6))
    kind = case['build']
    if kind == 'folder_raw':
        # MkPFS "pack folder --no-compress" (legacy .ffpfs and the nested image)
        build_pfs(source_root=Path(src), output_path=Path(out),
                  case_insensitive=case.get('case_insensitive', True), signed=False, compress=False,
                  threshold_gain=5, cpu_count=2, dry_run=False, verbose=False, zlib_level=int(level),
                  use_ram_if_possible=False, block_size=bs)
    elif kind == 'folder_compressed':
        # Lazy main_folder.py --no-exfat (per-file compression); two workers
        # (one worker crashes Lazy, see PFS.md).
        run_cli('main_folder', [src, out, '--no-exfat', '--no-ram', '--cpu-count', '2', '--zlib-level', level,
                                '--min-compress-size', str(case.get('min_compress_mb', 5.0)), '-q'])
    elif kind == 'file_container':
        run_cli('main_file', [os.path.join(src, case['file']), out, '--zlib-level', level, '--cpu-count', '2',
                              '--min-compress-size', str(case.get('min_compress_mb', 1.0)), '--no-ram', '-q'])
    elif kind == 'folder_container':
        # Legacy PFS tab two-step: folder -> uncompressed pfs_image.dat -> pack file.
        nested_dir = os.path.join(work, 'nested')
        os.makedirs(nested_dir, exist_ok=True)
        nested = os.path.join(nested_dir, 'pfs_image.dat')
        if os.path.exists(nested):
            os.remove(nested)
        build_pfs(source_root=Path(src), output_path=Path(nested), case_insensitive=True, signed=False,
                  compress=False, threshold_gain=5, cpu_count=2, dry_run=False, verbose=False,
                  zlib_level=int(level), use_ram_if_possible=False, block_size=bs)
        run_cli('main_file', [nested, out, '--zlib-level', level, '--cpu-count', '2', '--no-ram', '-q'])
    else:
        raise ValueError(kind)
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    scratch = os.path.abspath(sys.argv[1])
    write = '--write' in sys.argv[2:]
    sys.path.insert(0, LAZY)
    with open(SPEC) as f:
        spec = json.load(f)
    fixed = float(spec['fixed_time'])
    time.time = lambda: fixed  # Lazy stamps inodes with int(time.time())
    import io
    results = {}
    for case in spec['cases']:
        work = os.path.join(scratch, case['name'])
        os.makedirs(work, exist_ok=True)
        saved = sys.stdout
        sys.stdout = io.StringIO()  # Lazy prints progress bars
        try:
            out = build_case(case, spec, work)
        finally:
            log = sys.stdout.getvalue()
            sys.stdout = saved
        digest = sha256(out)
        size = os.path.getsize(out)
        results[case['name']] = {'sha256': digest, 'size': size}
        print(f"{case['name']:36s} {size:>10d}  {digest}")
        if write and size < 200 * 1024:
            os.makedirs(os.path.join(HERE, 'images'), exist_ok=True)
            shutil.copyfile(out, os.path.join(HERE, 'images', case['name'] + '.img'))
    if write:
        with open(os.path.join(LAZY, 'UPSTREAM_COMMIT.txt')) as f:
            commit = f.read().strip()
        doc = {
            'comment': 'Generated by make_golden.py from golden_cases.json with the vendored Lazy_MkPFS. '
                       'Do not edit by hand.',
            'lazy_mkpfs_commit': commit,
            'python': platform.python_version(),
            'zlib': zlib.ZLIB_RUNTIME_VERSION,
            'cases': results,
        }
        with open(os.path.join(HERE, 'golden.json'), 'w') as f:
            json.dump(doc, f, indent=2, sort_keys=True)
            f.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
