#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path
from lazy_mkpfs import pack_batch

def main():
    parser = argparse.ArgumentParser(description="Batch pack multiple game folders and files")
    parser.add_argument("input_folder", type=Path, help="Input folder containing games to pack")
    parser.add_argument("output_folder", type=Path, help="Output folder for packed images")
    
    parser.add_argument("--workers", type=int, default=None, help="Number of parallel workers")
    parser.add_argument("--cpu-count", type=int, default=0, help="CPU cores per task (0 = auto)")
    parser.add_argument("--no-ram", action="store_true", help="Disable RAM-based writing")
    
    parser.add_argument("--zlib-level", type=int, default=6, choices=range(1, 10))
    parser.add_argument("--zlib-ng", action="store_true", help="Use zlib-ng backend")
    parser.add_argument("--zlib-isa", action="store_true", help="Use Intel ISA-L backend")
    
    parser.add_argument("--no-exfat", action="store_true", help="Disable exFAT wrapper")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--quiet", "-q", action="store_true")
    args = parser.parse_args()

    if not args.input_folder.is_dir():
        print(f"Error: Input folder does not exist: {args.input_folder}", file=sys.stderr)
        sys.exit(1)

    args.output_folder.mkdir(parents=True, exist_ok=True)

    zlib_backend = "zlib"
    if args.zlib_isa:
        zlib_backend = "isa-l"
    elif args.zlib_ng:
        zlib_backend = "zlib-ng"

    try:
        results = pack_batch(
            input_dir=args.input_folder,
            output_dir=args.output_folder,
            workers=args.workers,
            zlib_backend=zlib_backend,
            zlib_level=args.zlib_level,
            cpu_count=args.cpu_count,
            use_ram_if_possible=not args.no_ram,
            verbose=args.verbose and not args.quiet,
            exfat=not args.no_exfat,
        )
        
        if not args.quiet:
            print("=" * 60)
            print("✅ Batch processing completed!")
            print(f"Total Processed : {results['total']}")
            print(f"Successful      : {results['succeeded']}")
            print(f"Failed          : {results['failed']}")
            if results['failed'] > 0:
                print("\n❌ Errors encountered:")
                for err in results['errors']:
                    print(f"  - {err}")
            print("=" * 60)
    except Exception as e:
        print(f"❌ Error during batch processing: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()