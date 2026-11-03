#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path
from lazy_mkpfs import pack_folder

def main():
    parser = argparse.ArgumentParser(description="Pack a game folder into a PS5 image")
    parser.add_argument("source_folder", type=Path, help="Source game folder to pack")
    parser.add_argument("output_image", type=Path, nargs="?", help="Output image path")
    
    parser.add_argument("--zlib-level", type=int, default=6, choices=range(1, 10))
    parser.add_argument("--min-compress-size", type=float, default=5.0)
    
    # NEW BACKEND FLAGS
    parser.add_argument("--zlib-ng", action="store_true", help="Use zlib-ng backend (faster)")
    parser.add_argument("--zlib-isa", action="store_true", help="Use Intel ISA-L backend (fastest)")
    
    parser.add_argument("--cpu-count", type=int, default=0)
    parser.add_argument("--no-ram", action="store_true")
    parser.add_argument("--no-exfat", action="store_true", help="Disable exFAT wrapper (creates standard .ffpfs instead of .ffpfsc)")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--quiet", "-q", action="store_true")
    args = parser.parse_args()

    if not args.source_folder.is_dir():
        print(f"Error: Source folder does not exist: {args.source_folder}", file=sys.stderr)
        sys.exit(1)

    # Resolve backend
    zlib_backend = "zlib"
    if args.zlib_isa:
        zlib_backend = "isa-l"
    elif args.zlib_ng:
        zlib_backend = "zlib-ng"

    try:
        stats = pack_folder(
            source_folder=args.source_folder, output_image=args.output_image, zlib_level=args.zlib_level,
            min_compress_size_mb=args.min_compress_size, zlib_backend=zlib_backend, cpu_count=args.cpu_count,
            use_ram_if_possible=not args.no_ram, verbose=args.verbose and not args.quiet, exfat=not args.no_exfat,
        )
        if not args.quiet:
            print("\n" + "=" * 60)
            print("✅ Packing completed successfully!")
            print(f"Total files:       {stats.total_files}")
            print(f"Compressed files:  {stats.compressed_files}")
            print(f"Uncompressed size: {stats.uncompressed_total_size:,} bytes")
            print(f"Stored size:       {stats.stored_total_size:,} bytes")
            print(f"Actual gain:       {stats.actual_gain_pct:.2f}%")
            print(f"Time elapsed:      {stats.elapsed_seconds:.2f} seconds")
            print("=" * 60)
    except Exception as e:
        print(f"❌ Error during packing: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()