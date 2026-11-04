#!/usr/bin/env python3
"""Time the vendored Lazy_MkPFS for tst_pfs_benchmark.

    python3 -I bench_lazy.py file   <source-file>   <output> <level> <cpu> <timestamp>
    python3 -I bench_lazy.py folder <source-folder> <output> <level> <cpu> <timestamp>

"file" is Lazy main_file.py (image -> .ffpfsc, the path the legacy app uses);
"folder" is the legacy PFS tab two-step (folder -> pfs_image.dat -> .ffpfsc).
Prints "seconds=<s>" for the build itself (interpreter start-up excluded).
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LAZY = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..', 'vendor', 'lazy_mkpfs'))


def main() -> int:
    mode, src, out, level, cpu, ts = sys.argv[1:7]
    sys.path.insert(0, LAZY)
    real_time = time.time
    fixed = float(ts)
    time.time = lambda: fixed  # Lazy stamps inodes with int(time.time())
    import io
    from pathlib import Path
    import main_file
    from lazy_mkpfs.build import build_pfs
    t0 = time.perf_counter()
    saved = sys.stdout
    sys.stdout = io.StringIO()
    try:
        if mode == 'folder':
            nested = os.path.join(os.path.dirname(out), 'pfs_image.dat')
            build_pfs(source_root=Path(src), output_path=Path(nested), case_insensitive=True, signed=False,
                      compress=False, threshold_gain=5, cpu_count=int(cpu), dry_run=False, verbose=False,
                      zlib_level=int(level), use_ram_if_possible=False, block_size=0x10000)
            src = nested
        sys.argv = ['main_file.py', src, out, '--zlib-level', level, '--cpu-count', cpu, '--no-ram', '-q']
        main_file.main()
    finally:
        sys.stdout = saved
    if mode == 'folder':
        os.remove(src)
    print(f"seconds={time.perf_counter() - t0:.3f}")
    time.time = real_time
    return 0


if __name__ == '__main__':
    sys.exit(main())
