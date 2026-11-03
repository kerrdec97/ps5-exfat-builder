from __future__ import annotations
import hashlib
import struct
import sys
import time
from pathlib import Path
from . import consts
from .inspect import parse_image_header, parse_image_inodes
from .compression import _parse_pfsc_header, get_zlib
from .crypto import parse_ekpfs_key_hex

def verify_pfs(
    image: str | Path,
    ekpfs: str | bytes | None = None,
    new_crypt: bool = False,
    verbose: bool = True,
) -> None:
    """Verify a PFS image and print the official MkPFS Check Report."""
    img_path = Path(image).resolve()
    ekpfs_bytes = None
    if ekpfs is not None:
        if isinstance(ekpfs, str):
            ekpfs_bytes = parse_ekpfs_key_hex(ekpfs)
        else:
            ekpfs_bytes = ekpfs

    errors = []
    warnings = []

    try:
        with img_path.open("rb") as f:
            header = parse_image_header(f)
            inodes = parse_image_inodes(f, header, ekpfs=ekpfs_bytes, new_crypt=new_crypt)
            
            num_inodes = len(inodes)
            num_dirs = 0
            num_files = 0
            num_compressed = 0
            
            crc32 = 0
            sha256 = hashlib.sha256()
            logical_bytes = 0
            stored_bytes = 0
             
            # Filter for actual file inodes (exclude internal ones)
            file_inodes = [
                i for i in inodes 
                if (i.mode & consts.INODE_MODE_FILE) and not (i.flags & consts.INODE_FLAG_INTERNAL)
            ]
            
            # Filter for actual directory inodes (exclude internal ones)
            dir_inodes = [
                i for i in inodes 
                if (i.mode & consts.INODE_MODE_DIR) and not (i.flags & consts.INODE_FLAG_INTERNAL)
            ]
            
            num_files = len(file_inodes)
            num_dirs = len(dir_inodes)
            file_inodes.sort(key=lambda x: x.number)
            
            # ─────────────────────────────────────────────────────────────
            # PRE-PASS: Calculate exact logical bytes for a smooth progress bar
            # ─────────────────────────────────────────────────────────────
            sys.stdout.write("\nAnalyzing image structure... ")
            sys.stdout.flush()
            total_bytes_to_verify = 0
            for inode in file_inodes:
                if inode.flags & consts.INODE_FLAG_COMPRESSED:
                    f.seek(inode.db[0] * header.block_size)
                    pfsc_header = f.read(consts.PFSC_HEADER_SIZE)
                    try:
                        _, _, _, _, logical_size = _parse_pfsc_header(pfsc_header)
                        total_bytes_to_verify += logical_size
                    except ValueError:
                        total_bytes_to_verify += inode.size
                else:
                    total_bytes_to_verify += inode.size
                    
            if total_bytes_to_verify == 0:
                total_bytes_to_verify = 1  # Prevent division by zero

            sys.stdout.write("Done.\nVerifying image payloads...\n")
            sys.stdout.flush()
            
            last_print_time = 0  # Throttle progress bar updates to max 10 per second
            
            # ─────────────────────────────────────────────────────────────
            # MAIN PASS: Stream, decompress, and hash block-by-block
            # ─────────────────────────────────────────────────────────────
            for idx, inode in enumerate(file_inodes):
                is_compressed = bool(inode.flags & consts.INODE_FLAG_COMPRESSED)
                if is_compressed:
                    num_compressed += 1
                    
                start_block = inode.db[0]
                f.seek(start_block * header.block_size)
                
                try:
                    if is_compressed:
                        pfsc_header = f.read(consts.PFSC_HEADER_SIZE)
                        logical_block_size, block_count, block_offsets_offset, data_offset, logical_size = _parse_pfsc_header(pfsc_header)
                         
                        offsets_size = (block_count + 1) * 8
                        f.seek(start_block * header.block_size + block_offsets_offset)
                        offsets_data = f.read(offsets_size)
                        offsets = list(struct.unpack_from(f"<{block_count + 1}Q", offsets_data, 0))
                        
                        actual_stored_size = offsets[-1]
                        stored_bytes += actual_stored_size
                        
                        for b_idx in range(block_count):
                            block_start = offsets[b_idx]
                            block_end = offsets[b_idx + 1]
                            block_len = block_end - block_start
                            
                            f.seek(start_block * header.block_size + block_start)
                            stored_block = f.read(block_len)
                            
                            if len(stored_block) == logical_block_size:
                                logical_block = stored_block
                            elif len(stored_block) < logical_block_size:
                                logical_block = get_zlib().decompress(stored_block)
                            else:
                                raise ValueError(f"PFSC block {b_idx} stored size {len(stored_block)} exceeds logical size {logical_block_size}")
                                
                            # Truncate the final block to the exact file size
                            if b_idx == block_count - 1:
                                expected_last_block_size = logical_size - (b_idx * logical_block_size)
                                if expected_last_block_size < len(logical_block):
                                    logical_block = logical_block[:expected_last_block_size]
                                    
                            crc32 = get_zlib().crc32(logical_block, crc32)
                            sha256.update(logical_block)
                            logical_bytes += len(logical_block)
                            
                            # Smooth inline progress bar (throttled)
                            now = time.time()
                            if now - last_print_time >= 0.1:
                                pct = min(1.0, logical_bytes / total_bytes_to_verify)
                                filled = int(40 * pct)
                                bar = "█" * filled + "░" * (40 - filled)
                                sys.stdout.write(f"\r[{bar}] {pct*100:5.1f}% verify")
                                sys.stdout.flush()
                                last_print_time = now
                        
                    else:
                        # Uncompressed: Stream and hash in 16MB chunks to save RAM
                        remaining = inode.size
                        stored_bytes += remaining
                        while remaining > 0:
                            chunk_size = min(16 * 1024 * 1024, remaining)
                            chunk = f.read(chunk_size)
                            if not chunk:
                                break
                            crc32 = get_zlib().crc32(chunk, crc32)
                            sha256.update(chunk)
                            logical_bytes += len(chunk)
                            remaining -= len(chunk)
                            
                            # Smooth inline progress bar (throttled)
                            now = time.time()
                            if now - last_print_time >= 0.1:
                                pct = min(1.0, logical_bytes / total_bytes_to_verify)
                                filled = int(40 * pct)
                                bar = "█" * filled + "░" * (40 - filled)
                                sys.stdout.write(f"\r[{bar}] {pct*100:5.1f}% verify")
                                sys.stdout.flush()
                                last_print_time = now
                            
                except Exception as e:
                    errors.append(f"Failed to process file inode {inode.number}: {e}")

            # Final 100% print with a newline to lock it in place
            sys.stdout.write(f"\r[{'█' * 40}] 100.0% verify\n")
            sys.stdout.flush()
            
            manifest_sha256 = sha256.hexdigest()
            fpt_keys = num_files + max(0, num_dirs - 1)

            if header.version not in (1, 2):
                warnings.append(f"Unusual PFS version: {header.version}")
            if header.readonly != 1:
                warnings.append("Image is not marked as read-only")

    except Exception as e:
        errors.append(f"Fatal verification error: {e}")

    # ─────────────────────────────────────────────────────────────
    # OFFICIAL MKPFS CHECK REPORT
    # ─────────────────────────────────────────────────────────────
    if verbose:
        version_str = f"{header.version} ({'PS5' if header.version == 2 else 'PS4'})"
        
        print("=" * 70)
        print("Lazy MkPFS 0.0.1 | https://github.com/Nazky/Lazy_MkPFS")
        print("=" * 70)
        print("Based on: https://github.com/PSBrew/MkPFS")
        print("=" * 70)
        print("PFS Check Report")
        print("=" * 70)
        print(f"Image:                 {img_path}")
        print(f"Version:               {version_str}")
        print(f"Header magic:          PFS ({header.magic})")
        print(f"Compression Setup:     PFSC (0x43534650)")
        print(f"Read-only:             {'yes' if header.readonly else 'no'}")
        print(f"Mode:                  0x{header.mode:04X}  (Bit 0=signed, Bit 1=64-bit inodes, Bit 2=encrypted, Bit 3=case insensitive)")
        print(f"  Signed:              {'yes' if header.mode & 0x1 else 'no'}")
        print(f"  64-bit inodes:       {'yes' if header.mode & 0x2 else 'no'}")
        print(f"  Encrypted:           {'yes' if header.mode & 0x4 else 'no'}")
        print(f"  Case insensitive:    {'yes' if header.mode & 0x8 else 'no'}")
        print(f"Block size:            {header.block_size:,} bytes")
        print(f"Inodes:                {num_inodes}")
        print(f"Directories:           {num_dirs}")
        print(f"Files:                 {num_files}")
        print(f"Compressed files:      {num_compressed}")
        print(f"Files hash-checked:    {num_files}")
        print(f"Data CRC32:            0x{crc32:08X}")
        print(f"Manifest SHA256:       {manifest_sha256}")
        print(f"Logical file bytes:    {logical_bytes:,}")
        print(f"Stored file bytes:     {stored_bytes:,}")
        print(f"flat_path_table keys:  {fpt_keys}")
        print(f"Warnings:              {len(warnings)}")
        print(f"Errors:                {len(errors)}")
        print("=" * 70)
        
        if errors:
            print("\n❌ ERRORS:")
            for err in errors:
                print(f"  - {err}")
                
        if warnings:
            print("\n⚠️ WARNINGS:")
            for warn in warnings:
                print(f"  - {warn}")
                
        if errors:
            raise SystemExit(1)