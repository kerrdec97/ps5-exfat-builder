# pfs/ampr_index.py
from __future__ import annotations
import os
import struct
from pathlib import Path

# Correct structures matching the official AMPRIDX3 format from build_ampr_index.py
RECORD_STRUCT = struct.Struct("<IIQq")       # offset (I), path_len (I), size (Q), mtime (q)
HASH_SLOT_STRUCT = struct.Struct("<QII")     # hash (Q), index+1 (I), flags (I)
HEADER_STRUCT = struct.Struct("<8sIIQQQII")  # magic, version, record_size, num_rows, path_blob_len, hash_offset, slot_size, num_slots

def key_for(path: str) -> str:
    return path.replace("\\", "/").lower()

def fnv1a64_path_hash(path: str) -> int:
    """Official FNV-1a 64-bit hash used by the APR resolver."""
    h = 1469598103934665603
    for ch in key_for(path):
        h ^= ord(ch)
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h or 1

def hash_slot_count(entry_count: int) -> int:
    if entry_count <= 0:
        return 0
    slots = 2
    target = entry_count * 2
    while slots < target:
        slots <<= 1
    return slots

def build_hash_slots(rows: list[tuple[int, int, str]]) -> list[tuple[int, int, int]]:
    """Build open-addressed hash slots with linear probing."""
    duplicate_flag = 1
    slots = [(0, 0, 0) for _ in range(hash_slot_count(len(rows)))]
    mask = len(slots) - 1
    
    for index, (_, _, path) in enumerate(rows):
        h = fnv1a64_path_hash(path)
        pos = h & mask
        
        while slots[pos][1] != 0:
            if slots[pos][0] == h:
                old_hash, old_index_plus_one, old_flags = slots[pos]
                slots[pos] = (old_hash, old_index_plus_one, old_flags | duplicate_flag)
            pos = (pos + 1) & mask
            
        slots[pos] = (h, index + 1, 0)
        
    return slots

def ensure_ampr_index(source_root: Path) -> None:
    """Check if ampr_emu.index exists. If not, and fakelib/libSceAmpr.sprx exists, generate it."""
    index_path = source_root / "ampr_emu.index"
    fakelib_sprx = source_root / "fakelib" / "libSceAmpr.sprx"

    if index_path.exists():
        return  # Already exists

    if fakelib_sprx.exists():
        print(f"🔧 Detected fakelib/libSceAmpr.sprx. Generating 'ampr_emu.index'...")
        try:
            build_index_local(source_root, index_path)
            print("✅ Successfully generated ampr_emu.index")
        except Exception as e:
            print(f"⚠️ Failed to generate index: {e}")

def build_index_local(root: Path, output_path: Path) -> None:
    """Build the AMPR index binary file using the official format."""
    root = root.resolve()
    output_path = output_path.resolve()
    output_tmp = output_path.with_suffix(output_path.suffix + ".tmp")
    
    seen: dict[str, str] = {}
    rows: list[tuple[int, int, str]] = []  # (size, mtime, indexed_path)
    
    # 1. Scan files
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort(key=str.lower)
        filenames.sort(key=str.lower)
        
        for filename in filenames:
            path = Path(dirpath) / filename
            try:
                resolved = path.resolve()
            except OSError:
                continue
                
            if resolved == output_path or resolved == output_tmp:
                continue
                
            rel = path.relative_to(root).as_posix()
            indexed_path = "/app0/" + rel
            
            # Skip the index file itself
            if indexed_path.lower() in ("/app0/ampr_emu.index", "/app0/ampr_emu.index.tmp"):
                continue
                
            try:
                st = path.stat()
            except OSError:
                continue
                
            if not path.is_file():
                continue
                
            # Handle case-insensitive collisions (keep first)
            key = key_for(indexed_path)
            if key in seen:
                continue
                
            seen[key] = indexed_path
            rows.append((st.st_size, int(st.st_mtime), indexed_path))
            
    if not rows:
        return
        
    # 2. Sort rows by path
    rows = sorted(rows, key=lambda row: key_for(row[2]))
    
    # 3. Build Path Blob and Records
    path_blob = bytearray()
    records = bytearray()
    
    for size, mtime, path in rows:
        encoded = path.encode("utf-8") + b"\0"
        offset = len(path_blob)
        path_len = len(encoded) - 1
        
        if offset > 0xFFFFFFFF or path_len > 0xFFFFFFFF:
            raise ValueError("index path blob is too large")
            
        # Pack using 64-bit size (Q) and 64-bit mtime (q) to support files > 4GB
        records += RECORD_STRUCT.pack(offset, path_len, size, mtime)
        path_blob += encoded
        
    # 4. Build Hash Slots
    hash_slots = build_hash_slots(rows)
    
    # 5. Calculate alignment and padding
    path_end = HEADER_STRUCT.size + len(records) + len(path_blob)
    hash_offset = (path_end + (HASH_SLOT_STRUCT.size - 1)) & ~(HASH_SLOT_STRUCT.size - 1)
    padding = b"\0" * (hash_offset - path_end)
    
    # 6. Write Final Binary
    with output_tmp.open("wb") as f:
        f.write(HEADER_STRUCT.pack(
            b"AMPRIDX3",
            3,
            RECORD_STRUCT.size,
            len(rows),
            len(path_blob),
            hash_offset,
            HASH_SLOT_STRUCT.size,
            len(hash_slots),
        ))
        f.write(records)
        f.write(path_blob)
        f.write(padding)
        
        for h, index_plus_one, flags in hash_slots:
            f.write(HASH_SLOT_STRUCT.pack(h, index_plus_one, flags))
            
    output_tmp.replace(output_path)