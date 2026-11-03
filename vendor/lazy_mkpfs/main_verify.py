#!/usr/bin/env python3
"""
Verify a PFS image and generate the official MkPFS Check Report.
Usage: python main_verify.py <image_path> [options]
"""
import argparse
import sys
from pathlib import Path
from lazy_mkpfs import verify_pfs

def main():
    parser = argparse.ArgumentParser(
        description="Verify a PFS image and generate the MkPFS Check Report",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python main_verify.py ./game.ffpfs
  python main_verify.py ./game.ffpfsc
        """
    )
    
    parser.add_argument("image_path", type=Path, help="Path to the PFS image (.ffpfs or .ffpfsc)")
    parser.add_argument("--ekpfs", type=str, help="EKPFS key in hex (for encrypted images)")
    parser.add_argument("--new-crypt", action="store_true", help="Use alternate newCrypt key derivation")
    parser.add_argument("--quiet", "-q", action="store_true", help="Suppress non-essential output")
    
    args = parser.parse_args()
    
    if not args.image_path.is_file():
        print(f"Error: Image file does not exist: {args.image_path}", file=sys.stderr)
        sys.exit(1)

    try:
        verify_pfs(
            image=args.image_path,
            ekpfs=args.ekpfs,
            new_crypt=args.new_crypt,
            verbose=not args.quiet,
        )
    except Exception as e:
        print(f"❌ Error during verification: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()