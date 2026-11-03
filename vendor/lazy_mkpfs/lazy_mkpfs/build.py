from __future__ import annotations
import os
import sys
import time
import shutil
import struct
import mmap
from typing import BinaryIO, cast
import multiprocessing as mp
from contextlib import suppress
from pathlib import Path

from . import consts
from .types import BuildError, Inode, SignatureTarget, BuildStats, FileNode, DirNode, ParsedHeader, Dirent, SignedInodeLayout, SupportsIntQueue
from .utils import ceil_div, human_readable_size, resolve_temp_root
from .crypto import pfs_gen_sign_key, hmac_sha256, encrypt_image_filesystem, read_image_bytes, resolve_ekpfs_key
from .compression import (
    write_source_to_blocks, write_source_to_offset, resolve_compression_worker_count, 
    cleanup_temporary_file_node_payloads, _compress_files_in_process, 
    _compute_file_storage_worker, _init_compression_worker, _drain_compression_progress_queue, 
    resolve_block_compression_worker_count, should_skip_executable_compression, 
    _encode_pfsc_into_handle, store_file_node_raw, _analyze_pfsc_file_storage, set_zlib_backend
)
from .logging import info
from .inspect import paths_have_fpt_collision, make_fpt_and_collision_blob, parse_image_header, parse_image_inode, parse_image_inodes
from .pbar import Progress

# CRITICAL LINUX FIX: Force 'spawn' to prevent C-extension deadlocks on fork()
if sys.platform == "linux":
    try:
        mp.set_start_method("spawn", force=True)
    except RuntimeError:
        pass

def estimate_file_data_footprint(*, file_sizes: list[int], block_size: int) -> int:
    return sum((ceil_div(size, block_size) * block_size) if size > 0 else block_size for size in file_sizes)

def choose_auto_fit_block_size(source_root: Path) -> int:
    file_sizes = [p.stat().st_size for p in source_root.rglob("*") if p.is_file()]
    if not file_sizes: return consts.PFSC_LOGICAL_BLOCK_SIZE
    candidates = (0x1000, 0x2000, 0x4000, 0x8000, 0x10000)
    return min(candidates, key=lambda c: (estimate_file_data_footprint(file_sizes=file_sizes, block_size=c), -c))

def validate_d32_ranges(inodes: list[Inode], final_ndblock: int) -> None:
    if final_ndblock > consts.INT32_MAX:
        raise BuildError(f"Image requires block index {final_ndblock}, exceeds D32 pointer limit {consts.INT32_MAX}")
    for ino in inodes:
        if not (0 <= ino.number <= consts.UINT32_MAX):
            raise BuildError(f"Inode number {ino.number} out of uint32 range")
        if not (0 <= ino.mode <= 0xFFFF):
            raise BuildError(f"Inode mode {ino.mode} out of uint16 range")
        if not (0 <= ino.nlink <= 0xFFFF):
            raise BuildError(f"Inode nlink {ino.nlink} out of uint16 range")
        if not (0 <= ino.flags <= consts.UINT32_MAX):
            raise BuildError(f"Inode flags {ino.flags} out of uint32 range")
        if not (0 <= ino.blocks <= consts.UINT32_MAX):
            raise BuildError(f"Inode blocks {ino.blocks} out of uint32 range")
        for ptr in ino.db:
            if not (-1 <= ptr <= consts.INT32_MAX):
                raise BuildError(f"Direct block pointer {ptr} out of int32 range")
        for ptr in ino.ib:
            if not (-1 <= ptr <= consts.INT32_MAX):
                raise BuildError(f"Indirect block pointer {ptr} out of int32 range")

def signed_inode_layout(inode_bits: int) -> SignedInodeLayout:
    if inode_bits == 32:
        return SignedInodeLayout(inode_size=consts.INODE_S32_SIZE, entry_size=consts.SIG_ENTRY_S32_SIZE, block_format="<i", pointer_table_offset=0x64)
    if inode_bits == 64:
        return SignedInodeLayout(inode_size=consts.INODE_S64_SIZE, entry_size=consts.SIG_ENTRY_S64_SIZE, block_format="<q", pointer_table_offset=0x68)
    raise BuildError(f"Unsupported signed inode width: {inode_bits}")

def signed_inode_bits_from_mode(mode: int) -> int:
    if mode & consts.PFS_MODE_64BIT_INODES:
        return 64
    return 32

def signed_inode_capacity_bytes(block_size: int, inode_bits: int) -> int:
    layout: SignedInodeLayout = signed_inode_layout(inode_bits)
    sigs_per_block: int = block_size // layout.entry_size
    if sigs_per_block <= 0:
        return 0
    max_blocks: int = 12 + sigs_per_block + (sigs_per_block * sigs_per_block)
    return max_blocks * block_size

def compose_pfs_mode(inode_bits: int, case_insensitive: bool) -> int:
    mode: int = 0
    if inode_bits == 64:
        mode |= consts.PFS_MODE_64BIT_INODES
    if case_insensitive:
        mode |= consts.PFS_MODE_CASE_INSENSITIVE
    return mode

def compose_pfs_mode_with_options(inode_bits: int, case_insensitive: bool, signed: bool, encrypted: bool) -> int:
    mode: int = compose_pfs_mode(inode_bits, case_insensitive)
    if signed:
        mode |= consts.PFS_MODE_SIGNED
    if encrypted:
        mode |= consts.PFS_MODE_ENCRYPTED
    return mode

def build_inode_block_sig_s64(inode_block_count: int, block_size: int, now: int, signed: bool = False) -> bytes:
    sig: bytearray = bytearray(0x310)
    struct.pack_into("<H", sig, 0x00, 0)
    struct.pack_into("<H", sig, 0x02, 1)
    struct.pack_into("<I", sig, 0x04, 0 if signed else consts.INODE_FLAG_READONLY)
    size_bytes: int = inode_block_count * block_size
    struct.pack_into("<q", sig, 0x08, size_bytes)
    struct.pack_into("<q", sig, 0x10, size_bytes)
    struct.pack_into("<qqqq", sig, 0x18, now, now, now, now)
    struct.pack_into("<IIII", sig, 0x38, 0, 0, 0, 0)
    struct.pack_into("<I", sig, 0x48, 0)
    struct.pack_into("<I", sig, 0x4C, 0)
    struct.pack_into("<Q", sig, 0x50, 0)
    struct.pack_into("<Q", sig, 0x58, 0)
    struct.pack_into("<I", sig, 0x60, inode_block_count)
    db_base: int = 0x68
    for i in range(12):
        block: int = 1 + i if i < inode_block_count else 0 if signed else (1 if i == 0 else 0)
        struct.pack_into("<q", sig, db_base + i * 40 + 32, block)
    ib_base: int = db_base + 12 * 40
    for i in range(5):
        struct.pack_into("<q", sig, ib_base + i * 40 + 32, 0)
    return bytes(sig)

def signed_inode_sig_offset(inode_number: int, ptr_index: int, block_size: int, inode_bits: int) -> int:
    layout: SignedInodeLayout = signed_inode_layout(inode_bits)
    inodes_per_block: int = block_size // layout.inode_size
    if inodes_per_block <= 0:
        raise BuildError("block size too small for signed inode table")
    inode_table_block: int = inode_number // inodes_per_block
    inode_index_in_block: int = inode_number % inodes_per_block
    inode_offset: int = block_size + (inode_table_block * block_size) + (inode_index_in_block * layout.inode_size)
    return inode_offset + layout.pointer_table_offset + (ptr_index * layout.entry_size)

def header_inode_block_sig_offset(ptr_index: int) -> int:
    return 0xB8 + (40 * ptr_index)

def make_sig_records_blob(blocks: list[int], block_size: int, inode_bits: int) -> bytes:
    layout: SignedInodeLayout = signed_inode_layout(inode_bits)
    blob: bytearray = bytearray(block_size)
    offset: int = 0
    for block in blocks:
        struct.pack_into(layout.block_format, blob, offset + consts.SIG_SIZE, block)
        offset += layout.entry_size
    return bytes(blob)

def collect_signed_block_numbers(inode: Inode, block_size: int, indirect_block_records: dict[int, list[int]], inode_bits: int) -> list[int]:
    layout: SignedInodeLayout = signed_inode_layout(inode_bits)
    sigs_per_block: int = block_size // layout.entry_size
    blocks: list[int] = []
    direct_count: int = min(inode.blocks, consts.MAX_DIRECT_BLOCKS)
    blocks.extend(inode.db[:direct_count])
    remaining: int = inode.blocks - direct_count
    if remaining > 0:
        ib0_children: list[int] = indirect_block_records.get(inode.ib[0], [])
        take: int = min(remaining, sigs_per_block)
        blocks.extend(ib0_children[:take])
        remaining -= take
    if remaining > 0:
        for child_indirect in indirect_block_records.get(inode.ib[1], []):
            child_children: list[int] = indirect_block_records.get(child_indirect, [])
            take = min(remaining, sigs_per_block)
            blocks.extend(child_children[:take])
            remaining -= take
            if remaining <= 0:
                break
    return blocks

def write_payload_to_blocks(out: BinaryIO, payload: bytes, blocks: list[int], block_size: int) -> None:
    for index, block in enumerate(blocks):
        chunk: bytes = payload[index * block_size : (index + 1) * block_size]
        if not chunk:
            break
        out.seek(block * block_size)
        out.write(chunk)

def assign_signed_inode_layout(inode: Inode, block_count: int, block_size: int, inode_bits: int, next_block: int, sig_targets: list[SignatureTarget], indirect_block_records: dict[int, list[int]]) -> int:
    layout: SignedInodeLayout = signed_inode_layout(inode_bits)
    sigs_per_block: int = block_size // layout.entry_size
    if sigs_per_block <= 0:
        raise BuildError("Block size too small for signed pointer records")
    if block_count > 12 + sigs_per_block + (sigs_per_block * sigs_per_block):
        raise BuildError(f"Signed inode {inode.number} requires {block_count} blocks, exceeds current signed layout capacity")

    for i in range(consts.MAX_DIRECT_BLOCKS):
        inode.db[i] = 0
    for i in range(consts.MAX_INDIRECT_BLOCKS):
        inode.ib[i] = 0

    direct_count = min(block_count, consts.MAX_DIRECT_BLOCKS)
    for i in range(direct_count):
        inode.db[i] = next_block
        sig_targets.append(SignatureTarget(next_block, signed_inode_sig_offset(inode.number, i, block_size, inode_bits), block_size, 0))
        next_block += 1

    remaining = block_count - direct_count
    if remaining <= 0:
        return next_block

    inode.ib[0] = next_block
    ib0_block = next_block
    next_block += 1
    sig_targets.append(SignatureTarget(ib0_block, signed_inode_sig_offset(inode.number, 12, block_size, inode_bits), block_size, 1))

    ib0_children: list[int] = []
    simple_count = min(remaining, sigs_per_block)
    for _ in range(simple_count):
        child_block = next_block
        next_block += 1
        ib0_children.append(child_block)
        sig_targets.append(SignatureTarget(child_block, ib0_block * block_size + len(ib0_children[:-1]) * layout.entry_size, block_size, 0))
    indirect_block_records[ib0_block] = ib0_children
    remaining -= simple_count
    if remaining <= 0:
        return next_block

    inode.ib[1] = next_block
    ib1_parent = next_block
    next_block += 1
    sig_targets.append(SignatureTarget(ib1_parent, signed_inode_sig_offset(inode.number, 13, block_size, inode_bits), block_size, 2))

    ib1_children: list[int] = []
    for idx in range(sigs_per_block):
        if remaining <= 0:
            break
        child_indirect_block = next_block
        next_block += 1
        ib1_children.append(child_indirect_block)
        sig_targets.append(SignatureTarget(child_indirect_block, ib1_parent * block_size + idx * layout.entry_size, block_size, 1))

        child_records: list[int] = []
        child_count = min(remaining, sigs_per_block)
        for rec_idx in range(child_count):
            data_block = next_block
            next_block += 1
            child_records.append(data_block)
            sig_targets.append(SignatureTarget(data_block, child_indirect_block * block_size + rec_idx * layout.entry_size, block_size, 0))
        indirect_block_records[child_indirect_block] = child_records
        remaining -= child_count

    indirect_block_records[ib1_parent] = ib1_children
    if remaining > 0:
        raise BuildError(f"Signed inode {inode.number} still has {remaining} blocks unallocated")

    return next_block

def _pack_pfs_header_block(*, block_size: int, pfs_version: int, mode: int, nblock: int, inode_count: int, final_ndblock: int, inode_block_count: int, now: int, signed: bool, encrypted: bool, seed: bytes) -> bytes:
    hdr: bytearray = bytearray(block_size)
    struct.pack_into("<q", hdr, 0x00, pfs_version)
    struct.pack_into("<q", hdr, 0x08, consts.PFS_MAGIC)
    struct.pack_into("<q", hdr, 0x10, 0)
    struct.pack_into("<BBBB", hdr, 0x18, 0, 0, 1, 0)
    struct.pack_into("<H", hdr, 0x1C, mode)
    struct.pack_into("<H", hdr, 0x1E, 0)
    struct.pack_into("<I", hdr, 0x20, block_size)
    struct.pack_into("<I", hdr, 0x24, 0)
    struct.pack_into("<q", hdr, 0x28, nblock)
    struct.pack_into("<q", hdr, 0x30, inode_count)
    struct.pack_into("<q", hdr, 0x38, final_ndblock)
    struct.pack_into("<q", hdr, 0x40, inode_block_count)
    ib_sig_bytes: bytes = build_inode_block_sig_s64(inode_block_count, block_size, now, signed=signed)
    hdr[0x50 : 0x50 + len(ib_sig_bytes)] = ib_sig_bytes
    if signed or encrypted:
        struct.pack_into("<I", hdr, 0x36C, 1)
        hdr[0x370 : 0x370 + len(seed)] = seed
    else:
        struct.pack_into("<I", hdr, 0x368, 1)
    return bytes(hdr)

def _write_inode_table(*, out: BinaryIO, inodes: list[Inode], signed: bool, signed_inode_bits: int, block_size: int, inode_size: int) -> None:
    for ino in inodes:
        if signed:
            out.write(ino.to_bytes_signed64() if signed_inode_bits == 64 else ino.to_bytes_signed32())
        else:
            out.write(ino.to_bytes())
        if (out.tell() % block_size) > (block_size - inode_size):
            out.seek(out.tell() + (block_size - (out.tell() % block_size)))

# --- LINUX OPTIMIZED SCANNING ---
def scan_source_tree(root: Path, progress: Progress) -> tuple[dict[str, DirNode], dict[str, FileNode], int]:
    progress.status("\nDiscovering files... ")
    root_str = str(root)
    raw_entries: list[tuple[str, str, int]] = []
    
    def _scan(dir_path: str, rel_dir: str):
        try:
            with os.scandir(dir_path) as it:
                for entry in it:
                    if entry.is_file():
                        rel_path = f"{rel_dir}/{entry.name}" if rel_dir else entry.name
                        raw_entries.append((rel_path, entry.path, entry.stat().st_size))
                    elif entry.is_dir():
                        sub_rel = f"{rel_dir}/{entry.name}" if rel_dir else entry.name
                        _scan(entry.path, sub_rel)
        except PermissionError:
            pass

    _scan(root_str, "")
    raw_entries.sort(key=lambda x: x[0].lower())
    
    non_ascii_paths: list[str] = []
    for rel_str, _, _ in raw_entries:
        if not rel_str.isascii():
            non_ascii_paths.append(rel_str)
            
    if non_ascii_paths:
        offenders = "\n   ".join(non_ascii_paths)
        raise BuildError(f"Source tree contains {len(non_ascii_paths)} file(s) with non-ASCII names. PFS images only support ASCII filenames:\n  {offenders}")

    dirs: dict[str, DirNode] = {"": DirNode(rel_dir="", name="uroot", parent_rel_dir=None)}
    files: dict[str, FileNode] = {}
    total_bytes: int = 0
    
    for i, (rel_str, abs_str, size) in enumerate(raw_entries, start=1):
        total_bytes += size
        if '/' in rel_str:
            parent, name = rel_str.rsplit('/', 1)
            parts = parent.split('/')
        else:
            parent = ""
            name = rel_str
            parts = []
            
        curr = ""
        for part in parts:
            next_rel = f"{curr}/{part}" if curr else part
            if next_rel not in dirs:
                dirs[next_rel] = DirNode(rel_dir=next_rel, name=part, parent_rel_dir=curr if curr != "" else "")
                dirs[curr].children_dirs.append(next_rel)
            curr = next_rel
            
        if parent not in dirs:
            dirs[parent] = DirNode(rel_dir=parent, name=Path(parent).name if parent else "uroot", parent_rel_dir="")
            
        abs_path = Path(abs_str)
        file_node = FileNode(rel_path=rel_str, abs_path=abs_path, parent_rel_dir=parent, name=name, raw_size=size)
        files[rel_str] = file_node
        dirs[parent].children_files.append(rel_str)
        progress.step("scan", i, len(raw_entries), bytes_processed=total_bytes)

    for d in dirs.values():
        d.children_dirs.sort(key=str.lower)
        d.children_files.sort(key=str.lower)

    return dirs, files, len(raw_entries)

def build_pfs(
    source_root: Path,
    output_path: Path,
    case_insensitive: bool,
    signed: bool,
    compress: bool,
    threshold_gain: int,
    cpu_count: int,
    dry_run: bool,
    verbose: bool,
    zlib_level: int = 4,
    encrypted: bool = False,
    new_crypt: bool = False,
    ekpfs: bytes | None = None,
    skip_executable_compression: bool = True,
    min_file_gain: int = 0,
    min_compress_size_mb: float = 0.0,
    temp_folder: Path | None = None,
    use_ram_if_possible: bool = True,
    zlib_backend: str = "zlib",  # UPDATED: Replaced use_zlib_ng
    block_size: int = 0x10000,
    pfs_version: int = consts.PFS_VERSION_PS5,
    inode_bits: int = 32,
) -> BuildStats:
    start: float = time.time()
    set_zlib_backend(zlib_backend)  # UPDATED
    progress: Progress = Progress(enabled=True)
    min_compress_size_bytes: int = int(min_compress_size_mb * 1024 * 1024)
    from .ampr_index import ensure_ampr_index
    ensure_ampr_index(source_root)
    temp_root: Path = resolve_temp_root(temp_folder=temp_folder, output_path=output_path)
    signed_inode_bits: int = 64 if signed and inode_bits == 64 else 32
    resolved_ekpfs: bytes = resolve_ekpfs_key(ekpfs=ekpfs)
    seed: bytes = consts.ZERO_PFS_SEED if (signed or encrypted) else b"\x00" * len(consts.ZERO_PFS_SEED)
    if not (0 <= min_file_gain <= 100):
        raise BuildError("min_file_gain must be within 0..100")
    if min_compress_size_bytes < 0:
        raise BuildError("min_compress_size_bytes must be non-negative")

    dirs, files, _ = scan_source_tree(source_root, progress)
    dir_nodes_sorted: list[DirNode] = sorted(dirs.values(), key=lambda d: d.rel_dir.lower())
    file_nodes_sorted: list[FileNode] = sorted(files.values(), key=lambda f: f.rel_path.lower())
    temporary_payload_paths: list[Path] = []

    compression_file_nodes: list[FileNode] = file_nodes_sorted
    if compress and skip_executable_compression:
        compression_file_nodes = []
        for f in file_nodes_sorted:
            if should_skip_executable_compression(file_name=f.name, file_path=f.rel_path):
                store_file_node_raw(f)
            else:
                compression_file_nodes.append(f)
    if compress and min_compress_size_bytes > 0:
        eligible_file_nodes: list[FileNode] = []
        for f in compression_file_nodes:
            if f.raw_size < min_compress_size_bytes:
                store_file_node_raw(f)
            else:
                eligible_file_nodes.append(f)
        compression_file_nodes = eligible_file_nodes

    if compress and len(compression_file_nodes) > 0:
        total_bytes_to_process: int = sum(f.raw_size for f in compression_file_nodes)
        compression_cpu_count: int = resolve_compression_worker_count(requested_cpu_count=cpu_count)
        worker_count: int = compression_cpu_count
        file_nodes_by_path: dict[Path, FileNode] = {f.abs_path: f for f in compression_file_nodes}
        progress.status(f"\nCompressing {len(compression_file_nodes)} files ({human_readable_size(total_bytes_to_process)}) using {worker_count} CPU core{'s' if worker_count != 1 else ''}... ")
        
        if worker_count == 1 or len(compression_file_nodes) == 1:
            try:
                _compress_files_in_process(
                    file_nodes_sorted=compression_file_nodes,
                    threshold_gain=threshold_gain,
                    min_file_gain=min_file_gain,
                    min_compress_size=min_compress_size_bytes,
                    zlib_level=zlib_level,
                    compression_cpu_count=compression_cpu_count,
                    dry_run=True,
                    total_bytes_to_process=total_bytes_to_process,
                    progress=progress,
                    temp_folder=None,
                )
            except OSError:
                raise
        else: 
            progress_total_units: int = total_bytes_to_process if total_bytes_to_process > 0 else len(compression_file_nodes)
            progress.step("compress", 0, progress_total_units, bytes_processed=0)
            total_bytes_processed: int = 0
            displayed_progress_units: int = 0
            
            # LINUX OPTIMIZATION: Fast OS-pipe queue injected via initializer
            progress_queue = cast(SupportsIntQueue, mp.Queue())
            worker_args: list[tuple[Path, int, int, int, bool, int, int, bool, Path | None]] = [
                (f.abs_path, threshold_gain, min_file_gain, min_compress_size_bytes, True, consts.PFSC_LOGICAL_BLOCK_SIZE, zlib_level, dry_run, temp_root)
                for f in compression_file_nodes
            ]
            
            with mp.Pool(
                processes=worker_count,
                initializer=_init_compression_worker,
                initargs=(progress_queue,)
            ) as pool:
                # chunksize=1 is REQUIRED for Python 3.12+ to support .next(timeout)
                results = pool.imap_unordered(_compute_file_storage_worker, worker_args, chunksize=1)
                remaining_results: int = len(worker_args)
                
                while remaining_results > 0:
                    queued_bytes: int = _drain_compression_progress_queue(progress_queue=progress_queue)
                    if queued_bytes > 0:
                        displayed_progress_units = min(total_bytes_to_process, displayed_progress_units + queued_bytes)
                        progress.step("compress", displayed_progress_units, progress_total_units, bytes_processed=displayed_progress_units if total_bytes_to_process > 0 else 0)
                    try:
                        # Increased timeout to 0.5s to reduce CPU context switching on Linux
                        result = results.next(timeout=0.5)
                    except mp.TimeoutError:
                        continue
                    except OSError:
                        cleanup_temporary_file_node_payloads(file_nodes=compression_file_nodes)
                        raise 

                    remaining_results -= 1
                    (abs_path, stored_source_path, stored_source_is_temp, stored_size, is_compressed, gain_pct, hyp_comp_size) = result
                    
                    file_node = file_nodes_by_path[abs_path]
                    file_node.stored_source_path = stored_source_path
                    file_node.stored_source_is_temp = stored_source_is_temp
                    file_node.stored_size = stored_size
                    file_node.compressed = is_compressed
                    file_node.gain_pct = gain_pct
                    file_node.hypothetical_compressed_size = hyp_comp_size
                    total_bytes_processed += file_node.raw_size
                    
                    completed_files: int = len(worker_args) - remaining_results
                    target_progress_units: int = total_bytes_processed if total_bytes_to_process > 0 else completed_files
                    if displayed_progress_units < target_progress_units:
                        displayed_progress_units = target_progress_units
                        progress.step("compress", displayed_progress_units, progress_total_units, bytes_processed=displayed_progress_units if total_bytes_to_process > 0 else 0)
                        
        if displayed_progress_units < progress_total_units:
            progress.step("compress", progress_total_units, progress_total_units, bytes_processed=total_bytes_to_process if total_bytes_to_process > 0 else 0)
            
        if not dry_run:
            temporary_payload_paths.extend([file_node.stored_source_path for file_node in compression_file_nodes if file_node.stored_source_is_temp and file_node.stored_source_path is not None])
    else:
        if len(file_nodes_sorted) > 0:
            total_bytes_to_process = sum(f.raw_size for f in file_nodes_sorted)
            progress.status(f"\nReading {len(file_nodes_sorted)} files ({human_readable_size(total_bytes_to_process)})... ")
            total_bytes_processed = 0
            for idx, f in enumerate(file_nodes_sorted, start=1):
                f.stored_source_path = f.abs_path
                f.stored_source_is_temp = False
                f.stored_size = f.raw_size
                f.compressed = False
                f.gain_pct = 0.0
                f.hypothetical_compressed_size = 0
                total_bytes_processed += f.raw_size
                progress.step("read", total_bytes_processed, total_bytes_to_process, bytes_processed=total_bytes_processed)

    now: int = int(time.time())
    inodes: list[Inode] = []

    super_root_inode = Inode(number=0, mode=consts.INODE_MODE_DIR | consts.INODE_RX_ONLY, nlink=1, flags=consts.INODE_FLAG_INTERNAL | (0 if signed else consts.INODE_FLAG_READONLY) | (consts.INODE_FLAG_SIGNED_EXTRA if signed else 0), size=block_size, size_compressed=block_size, blocks=1, time_sec=now)
    fpt_inode = Inode(number=1, mode=consts.INODE_MODE_FILE | consts.INODE_RX_ONLY, nlink=1, flags=consts.INODE_FLAG_INTERNAL | (0 if signed else consts.INODE_FLAG_READONLY) | (consts.INODE_FLAG_SIGNED_EXTRA if signed else 0), size=0, size_compressed=0, blocks=1, time_sec=now)
    collision_inode: Inode | None = None
    uroot_inode_num = 2
    uroot_inode = Inode(number=uroot_inode_num, mode=consts.INODE_MODE_DIR | consts.INODE_RX_ONLY, nlink=3, flags=(0 if signed else consts.INODE_FLAG_READONLY) | (consts.INODE_FLAG_SIGNED_EXTRA if signed else 0), size=block_size, size_compressed=block_size, blocks=1, time_sec=now)

    inodes.extend([super_root_inode, fpt_inode, uroot_inode])
    dirs[""].inode = uroot_inode
    inode_by_path: dict[str, Inode] = {"dir:": uroot_inode}
    next_inode_number = 3

    non_root_dirs = [d for d in dir_nodes_sorted if d.rel_dir != ""]
    for d in non_root_dirs:
        ino = Inode(number=next_inode_number, mode=consts.INODE_MODE_DIR | consts.INODE_RX_ONLY, nlink=2, flags=consts.INODE_FLAG_READONLY | (consts.INODE_FLAG_SIGNED_EXTRA if signed else 0), size=block_size, size_compressed=block_size, blocks=1, time_sec=now)
        d.inode = ino
        inode_by_path[f"dir:{d.rel_dir}"] = ino
        inodes.append(ino)
        next_inode_number += 1

    for f in file_nodes_sorted:
        flags = consts.INODE_FLAG_READONLY | (consts.INODE_FLAG_COMPRESSED if f.compressed else 0) | (consts.INODE_FLAG_SIGNED_EXTRA if signed else 0)
        blocks = max(1, ceil_div(f.stored_size, block_size)) if f.stored_size > 0 else 1
        file_size = f.stored_size
        file_size_compressed = (ceil_div(f.raw_size, block_size) * block_size) if f.compressed else f.stored_size
        ino = Inode(number=next_inode_number, mode=consts.INODE_MODE_FILE | consts.INODE_RX_ONLY, nlink=1, flags=flags, size=file_size, size_compressed=file_size_compressed, blocks=blocks, time_sec=now)
        f.inode = ino
        inode_by_path[f"file:{f.rel_path}"] = ino
        inodes.append(ino)
        next_inode_number += 1

    for d in dir_nodes_sorted:
        parent_ino = inode_by_path["dir:" + (d.parent_rel_dir if d.parent_rel_dir is not None else "")]
        this_ino = inode_by_path["dir:" + d.rel_dir]
        d.dirents = [Dirent(this_ino.number, consts.DIRENT_TYPE_DOT, "."), Dirent(parent_ino.number if d.rel_dir != "" else this_ino.number, consts.DIRENT_TYPE_DOTDOT, "..")]
        for child_rel_dir in d.children_dirs:
            child_dir = dirs[child_rel_dir]
            d.dirents.append(Dirent(child_dir.inode.number, consts.DIRENT_TYPE_DIRECTORY, child_dir.name))
            this_ino.nlink += 1
        for child_rel_file in d.children_files:
            child_file = files[child_rel_file]
            d.dirents.append(Dirent(child_file.inode.number, consts.DIRENT_TYPE_FILE, child_file.name))

    has_collision: bool = paths_have_fpt_collision(dir_nodes_sorted, file_nodes_sorted, case_insensitive=case_insensitive)
    fpt_blob: bytes = b""
    collision_blob: bytes | None = None

    if has_collision:
        collision_inode_number: int = next_inode_number
        collision_inode = Inode(number=collision_inode_number, mode=consts.INODE_MODE_FILE | consts.INODE_RX_ONLY, nlink=1, flags=consts.INODE_FLAG_INTERNAL | (0 if signed else consts.INODE_FLAG_READONLY) | (consts.INODE_FLAG_SIGNED_EXTRA if signed else 0), size=0, size_compressed=0, blocks=1, time_sec=now)
        inodes = [super_root_inode, fpt_inode, collision_inode, uroot_inode] + [ino for ino in inodes if ino.number >= 3]
        remap: dict[int, int] = {}
        for idx, ino in enumerate(inodes):
            old = ino.number
            ino.number = idx
            remap[old] = idx
        for d in dir_nodes_sorted:
            for ent in d.dirents:
                ent.inode_number = remap[ent.inode_number]
        inode_by_path = {}
        for d in dir_nodes_sorted:
            inode_by_path[f"dir:{d.rel_dir}"] = d.inode
        for f in file_nodes_sorted:
            inode_by_path[f"file:{f.rel_path}"] = f.inode

    fpt_blob, collision_blob, has_collision = make_fpt_and_collision_blob(dir_nodes_sorted, file_nodes_sorted, inode_by_path, case_insensitive=case_insensitive)
    if collision_inode is not None:
        collision_blob_size: int = len(collision_blob or b"")
        collision_inode.size = collision_blob_size
        collision_inode.size_compressed = collision_blob_size
        collision_inode.blocks = max(1, ceil_div(collision_blob_size, block_size))

    super_root_dirents: list[Dirent] = [Dirent(fpt_inode.number, consts.DIRENT_TYPE_FILE, "flat_path_table")]
    if has_collision and collision_inode is not None:
        super_root_dirents.append(Dirent(collision_inode.number, consts.DIRENT_TYPE_FILE, "collision_resolver"))
    super_root_dirents.append(Dirent(uroot_inode.number, consts.DIRENT_TYPE_DIRECTORY, "uroot"))

    inode_count = len(inodes)
    inode_size: int = signed_inode_layout(signed_inode_bits).inode_size if signed else consts.INODE_D32_SIZE
    inodes_per_block = block_size // inode_size
    inode_block_count = ceil_div(inode_count, inodes_per_block)

    all_nodes_data: list[tuple[Inode, int, bool, bytes | None, Path | None]] = []
    root_blob = b"".join(d.to_bytes() for d in dirs[""].dirents)
    all_nodes_data.append((dirs[""].inode, len(root_blob), True, root_blob, None))
    for d in non_root_dirs:
        blob = b"".join(ent.to_bytes() for ent in d.dirents)
        all_nodes_data.append((d.inode, len(blob), True, blob, None))
    for f in file_nodes_sorted:
        if f.stored_source_path is None:
            raise BuildError(f"Internal error: missing stored payload source for {f.rel_path}")
        all_nodes_data.append((f.inode, f.stored_size, False, None, f.stored_source_path))

    signature_targets: list[SignatureTarget] = []
    indirect_block_records: dict[int, list[int]] = {}
    reserved_empty_blocks: set[int] = set()

    if signed:
        max_signed_size: int = signed_inode_capacity_bytes(block_size, signed_inode_bits)
        if max_signed_size <= 0:
            raise BuildError("Block size too small for signed PFS layout")
        for f in file_nodes_sorted:
            if f.stored_size > max_signed_size:
                raise BuildError(f"Signed mode cannot represent file '{f.rel_path}' with block size {block_size}; max supported stored payload is {max_signed_size} bytes")
        ndblock = 1
        for i in range(inode_block_count):
            signature_targets.append(SignatureTarget(1 + i, header_inode_block_sig_offset(i), block_size, 3))
        ndblock += inode_block_count
        super_root_inode.blocks = 1
        ndblock = assign_signed_inode_layout(super_root_inode, super_root_inode.blocks, block_size, signed_inode_bits, ndblock, signature_targets, indirect_block_records)
        fpt_inode.size = len(fpt_blob)
        fpt_inode.size_compressed = len(fpt_blob)
        fpt_inode.blocks = max(1, ceil_div(len(fpt_blob), block_size))
        ndblock = assign_signed_inode_layout(fpt_inode, fpt_inode.blocks, block_size, signed_inode_bits, ndblock, signature_targets, indirect_block_records)
        if has_collision and collision_inode is not None:
            ndblock = assign_signed_inode_layout(collision_inode, collision_inode.blocks, block_size, signed_inode_bits, ndblock, signature_targets, indirect_block_records)
        ndblock += 2
        reserved_empty_blocks.update({ndblock - 2, ndblock - 1})
        for inode, payload_size, is_dir, _payload_bytes, _payload_source in all_nodes_data:
            blocks = max(1, ceil_div(payload_size, block_size)) if payload_size > 0 else 1
            inode.blocks = blocks
            if is_dir:
                inode.size = blocks * block_size
                inode.size_compressed = inode.size
            else:
                if inode.flags & consts.INODE_FLAG_COMPRESSED:
                    inode.size = payload_size
                else:
                    inode.size = payload_size
                    inode.size_compressed = inode.size
            ndblock = assign_signed_inode_layout(inode, blocks, block_size, signed_inode_bits, ndblock, signature_targets, indirect_block_records)
        signature_targets.append(SignatureTarget(0, consts.HEADER_DIGEST_OFFSET, consts.HEADER_DIGEST_SIZE, 4))
    else:
        ndblock = 1
        ndblock += inode_block_count
        super_root_inode.db[0] = ndblock
        ndblock += super_root_inode.blocks
        fpt_inode.size = len(fpt_blob)
        fpt_inode.size_compressed = len(fpt_blob)
        fpt_inode.blocks = max(1, ceil_div(len(fpt_blob), block_size))
        fpt_inode.db[0] = ndblock
        for i in range(1, consts.MAX_DIRECT_BLOCKS):
            fpt_inode.db[i] = -1
        ndblock += fpt_inode.blocks
        if has_collision and collision_inode is not None:
            collision_inode.db[0] = ndblock
            for i in range(1, consts.MAX_DIRECT_BLOCKS):
                collision_inode.db[i] = -1
            ndblock += collision_inode.blocks
        else:
            ndblock += 1
            reserved_empty_blocks.add(ndblock - 1)
        for inode, payload_size, is_dir, _payload_bytes, _payload_source in all_nodes_data:
            blocks = max(1, ceil_div(payload_size, block_size)) if payload_size > 0 else 1
            inode.db[0] = ndblock
            inode.blocks = blocks
            for i in range(1, consts.MAX_DIRECT_BLOCKS):
                inode.db[i] = -1
            if is_dir:
                inode.size = blocks * block_size
                inode.size_compressed = inode.size
            else:
                if inode.flags & consts.INODE_FLAG_COMPRESSED:
                    inode.size = payload_size
                else:
                    inode.size = payload_size
                    inode.size_compressed = inode.size
            ndblock += blocks

    nblock = 1
    final_ndblock = ndblock
    validate_d32_ranges(inodes, final_ndblock)

    stats = BuildStats(input_path=source_root, output_path=output_path)
    stats.total_files = len(file_nodes_sorted)
    stats.uncompressed_total_size = sum(f.raw_size for f in file_nodes_sorted)
    stats.stored_total_size = sum(f.stored_size for f in file_nodes_sorted)
    stats.all_compressed_total_size = sum(f.hypothetical_compressed_size for f in file_nodes_sorted)
    stats.compressed_files = sum(1 for f in file_nodes_sorted if f.compressed)
    stats.uncompressed_files = stats.total_files - stats.compressed_files
    stats.block_size = block_size
    stats.block_alignment_waste = sum((ceil_div(f.stored_size, block_size) * block_size - f.stored_size) if f.stored_size > 0 else block_size for f in file_nodes_sorted)
    
    if verbose:
        for f in file_nodes_sorted:
            state: str = "compressed" if f.compressed else "raw"
            info(f"[file] {f.rel_path}: raw={f.raw_size} stored={f.stored_size} gain={f.gain_pct:.2f}% mode={state}", icon_name="file")

    if dry_run:
        stats.elapsed_seconds = time.time() - start
        return stats

    mode = compose_pfs_mode_with_options(inode_bits=inode_bits, case_insensitive=case_insensitive, signed=signed, encrypted=encrypted)
    progress.status("\nPreparing to write PFS image... ")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    image_size = final_ndblock * block_size

    if use_ram_if_possible:
        from .utils import get_available_ram_bytes
        free_ram = get_available_ram_bytes()
        max_ram_usage = int(free_ram * 0.8)
        if free_ram > 0 and image_size <= max_ram_usage:
            import tempfile
            out = tempfile.SpooledTemporaryFile(max_size=max_ram_usage, mode="w+b")
            is_spooled = True
            tmp_path = None
            progress.status(f"Writing PFS image to RAM (up to {human_readable_size(max_ram_usage)}) before saving to {output_path}... ")
        else:
            tmp_path = Path(str(output_path) + ".tmp")
            out = open(tmp_path, "w+b", buffering=64 * 1024 * 1024)
            is_spooled = False
            progress.status(f"Writing PFS image to disk (buffered) at {tmp_path}... ")
    else:
        tmp_path = Path(str(output_path) + ".tmp")
        out = open(tmp_path, "w+b", buffering=64 * 1024 * 1024)
        is_spooled = False
        progress.status(f"Writing PFS image to disk at {tmp_path}... ")

    try:
        if not is_spooled:
            out.truncate(image_size)
        hdr = _pack_pfs_header_block(block_size=block_size, pfs_version=pfs_version, mode=mode, nblock=nblock, inode_count=inode_count, final_ndblock=final_ndblock, inode_block_count=inode_block_count, now=now, signed=signed, encrypted=encrypted, seed=seed)
        out.seek(0)
        out.write(hdr)
        out.seek(block_size)
        _write_inode_table(out=out, inodes=inodes, signed=signed, signed_inode_bits=signed_inode_bits, block_size=block_size, inode_size=inode_size)
        out.seek(block_size * (inode_block_count + 1))
        for d in super_root_dirents:
            out.write(d.to_bytes())
        if signed:
            write_payload_to_blocks(out, fpt_blob, collect_signed_block_numbers(fpt_inode, block_size, indirect_block_records, signed_inode_bits), block_size)
            if has_collision and collision_inode is not None and collision_blob is not None:
                write_payload_to_blocks(out, collision_blob, collect_signed_block_numbers(collision_inode, block_size, indirect_block_records, signed_inode_bits), block_size)
            for block, records in indirect_block_records.items():
                out.seek(block * block_size)
                out.write(make_sig_records_blob(records, block_size, signed_inode_bits))
        else:
            out.seek(fpt_inode.db[0] * block_size)
            out.write(fpt_blob)
            if has_collision and collision_inode is not None and collision_blob is not None:
                out.seek(collision_inode.db[0] * block_size)
                out.write(collision_blob)

        total_write_bytes: int = sum(payload_size for _inode, payload_size, _is_dir, _bytes, _path in all_nodes_data)
        written_bytes: int = 0
        
        def report_write(delta: int) -> None:
            nonlocal written_bytes
            written_bytes += delta
            progress.step("write", written_bytes, total_write_bytes, bytes_processed=written_bytes)

        for inode, payload_size, _is_dir, payload_bytes, payload_source_path in all_nodes_data:
            if payload_bytes is not None:
                if signed:
                    write_payload_to_blocks(out, payload_bytes, collect_signed_block_numbers(inode, block_size, indirect_block_records, signed_inode_bits), block_size)
                else:
                    out.seek(inode.db[0] * block_size)
                    out.write(payload_bytes)
                report_write(payload_size)
            else:
                if payload_source_path is None:
                    raise BuildError(f"Internal error: payload source is missing for inode {inode.number}")
                if signed:
                    write_source_to_blocks(out=out, source_path=payload_source_path, payload_size=payload_size, blocks=collect_signed_block_numbers(inode, block_size, indirect_block_records, signed_inode_bits), block_size=block_size, progress_callback=report_write)
                else:
                    write_source_to_offset(out=out, source_path=payload_source_path, payload_size=payload_size, offset=inode.db[0] * block_size, progress_callback=report_write)

        if signed:
            sign_key = pfs_gen_sign_key(resolved_ekpfs, seed)
            for level in range(5):
                for target in (t for t in signature_targets if t.level == level):
                    block_data = bytearray(read_image_bytes(out, target.block * block_size, target.size))
                    sig_pos_in_block = target.sig_offset - (target.block * block_size)
                    if 0 <= sig_pos_in_block <= len(block_data) - consts.SIG_SIZE:
                        block_data[sig_pos_in_block : sig_pos_in_block + consts.SIG_SIZE] = b"\x00" * consts.SIG_SIZE
                    out.seek(target.sig_offset)
                    out.write(hmac_sha256(sign_key, bytes(block_data)))

        if encrypted:
            encrypt_image_filesystem(out, block_size=block_size, total_blocks=final_ndblock, ekpfs=resolved_ekpfs, seed=seed, new_crypt=new_crypt, skip_block_numbers=reserved_empty_blocks)

        if is_spooled:
            out.seek(0, 2) 
            spool_size = out.tell()
            out.seek(0)
            progress.status("\nFlushing RAM to disk... ")
            flushed_bytes = 0
            with output_path.open("wb") as final_out:
                while True:
                    chunk = out.read(32 * 1024 * 1024)
                    if not chunk:
                        break
                    final_out.write(chunk)
                    flushed_bytes += len(chunk)
                    progress.step("save", flushed_bytes, spool_size, bytes_processed=flushed_bytes)
            out.close()
            validate_image_quick(output_path, block_size, mode, pfs_version, ekpfs=resolved_ekpfs if encrypted else None, new_crypt=new_crypt)
        else:
            out.close()
            validate_image_quick(tmp_path, block_size, mode, pfs_version, ekpfs=resolved_ekpfs if encrypted else None, new_crypt=new_crypt)
            shutil.move(str(tmp_path), str(output_path))

        for temporary_payload_path in temporary_payload_paths:
            with suppress(FileNotFoundError):
                temporary_payload_path.unlink()
        progress.status(f"Successfully wrote {human_readable_size(image_size)} image")

    except Exception:
        if not is_spooled and tmp_path and tmp_path.exists():
            with suppress(FileNotFoundError):
                tmp_path.unlink()
        for temporary_payload_path in temporary_payload_paths:
            with suppress(FileNotFoundError):
                temporary_payload_path.unlink()
        raise

    stats.elapsed_seconds = time.time() - start
    return stats

def _single_file_build_stats(*, source_file: Path, output_path: Path, raw_size: int, stored_size: int, is_compressed: bool, hypothetical_size: int, block_size: int, gain_pct: float, elapsed_seconds: float, verbose: bool) -> BuildStats:
    stats = BuildStats(input_path=source_file, output_path=output_path)
    stats.total_files = 1
    stats.uncompressed_total_size = raw_size
    stats.stored_total_size = stored_size
    stats.all_compressed_total_size = hypothetical_size
    stats.compressed_files = 1 if is_compressed else 0
    stats.uncompressed_files = 0 if is_compressed else 1
    stats.block_size = block_size
    stats.block_alignment_waste = (ceil_div(stored_size, block_size) * block_size - stored_size) if stored_size > 0 else block_size
    stats.elapsed_seconds = elapsed_seconds
    if verbose:
        state: str = "compressed" if is_compressed else "raw"
        info(f"[file] {source_file.name}: raw={raw_size} stored={stored_size} gain={gain_pct:.2f}% mode={state}", icon_name="file")
    return stats

def build_pfs_stream_single_file(
    *,
    source_file: Path,
    output_path: Path,
    block_size: int,
    pfs_version: int,
    case_insensitive: bool,
    zlib_level: int = 4,
    threshold_gain: int,
    min_file_gain: int,
    min_compress_size_mb: float = 0.0,
    cpu_count: int,
    compress: bool,
    encrypted: bool = False,
    new_crypt: bool = False,
    ekpfs: bytes | None = None,
    verbose: bool = False,
    skip_executable_compression: bool = False,
    dry_run: bool = False,
    use_ram_if_possible: bool = True,
    zlib_backend: str = "zlib",  # UPDATED: Replaced use_zlib_ng
) -> BuildStats:
    start: float = time.time()
    set_zlib_backend(zlib_backend)  # UPDATED
    progress: Progress = Progress(enabled=True)
    min_compress_size_bytes: int = int(min_compress_size_mb * 1024 * 1024)
    if not source_file.is_file():
        raise BuildError(f"source file does not exist: {source_file}")
    raw_size: int = source_file.stat().st_size
    now: int = int(time.time())
    resolved_ekpfs: bytes = resolve_ekpfs_key(ekpfs=ekpfs)
    seed: bytes = consts.ZERO_PFS_SEED

    should_compress: bool = (
        compress and raw_size > 0 and raw_size >= min_compress_size_bytes
        and not (skip_executable_compression and should_skip_executable_compression(file_name=source_file.name, file_path=source_file.name))
    )
    block_workers: int = resolve_block_compression_worker_count(
        requested_cpu_count=resolve_compression_worker_count(requested_cpu_count=cpu_count), file_size=raw_size
    )

    if dry_run:
        if should_compress:
            dry_stored, dry_compressed, dry_gain, dry_hyp = _analyze_pfsc_file_storage(
                abs_path=source_file, threshold_gain=threshold_gain, min_file_gain=min_file_gain, zlib_level=zlib_level,
                logical_block_size=consts.PFSC_LOGICAL_BLOCK_SIZE, block_worker_count=block_workers,
            )
        else:
            dry_stored, dry_compressed, dry_gain, dry_hyp = raw_size, False, 0.0, 0
        return _single_file_build_stats(
            source_file=source_file, output_path=output_path, raw_size=raw_size, stored_size=dry_stored,
            is_compressed=dry_compressed, hypothetical_size=dry_hyp, block_size=block_size, gain_pct=dry_gain,
            elapsed_seconds=time.time() - start, verbose=verbose,
        )

    super_root_inode = Inode(number=0, mode=consts.INODE_MODE_DIR | consts.INODE_RX_ONLY, nlink=1, flags=consts.INODE_FLAG_INTERNAL | consts.INODE_FLAG_READONLY, size=block_size, size_compressed=block_size, blocks=1, time_sec=now)
    fpt_inode = Inode(number=1, mode=consts.INODE_MODE_FILE | consts.INODE_RX_ONLY, nlink=1, flags=consts.INODE_FLAG_INTERNAL | consts.INODE_FLAG_READONLY, size=0, size_compressed=0, blocks=1, time_sec=now)
    uroot_inode = Inode(number=2, mode=consts.INODE_MODE_DIR | consts.INODE_RX_ONLY, nlink=3, flags=consts.INODE_FLAG_READONLY, size=block_size, size_compressed=block_size, blocks=1, time_sec=now)
    file_inode = Inode(number=3, mode=consts.INODE_MODE_FILE | consts.INODE_RX_ONLY, nlink=1, flags=consts.INODE_FLAG_READONLY, size=0, size_compressed=0, blocks=1, time_sec=now)
    inodes: list[Inode] = [super_root_inode, fpt_inode, uroot_inode, file_inode]

    file_node = FileNode(rel_path=source_file.name, abs_path=source_file, parent_rel_dir="", name=source_file.name, raw_size=raw_size)
    file_node.inode = file_inode
    uroot_dir = DirNode(rel_dir="", name="", parent_rel_dir=None, children_files=[source_file.name])
    uroot_dir.inode = uroot_inode
    inode_by_path: dict[str, Inode] = {"dir:": uroot_inode, f"file:{source_file.name}": file_inode}

    fpt_blob: bytes
    has_collision: bool
    fpt_blob, _collision_blob, has_collision = make_fpt_and_collision_blob([uroot_dir], [file_node], inode_by_path, case_insensitive=case_insensitive)
    if has_collision:
        raise BuildError("unexpected FPT collision in single-file streaming builder")

    uroot_dirents: list[Dirent] = [
        Dirent(uroot_inode.number, consts.DIRENT_TYPE_DOT, "."),
        Dirent(uroot_inode.number, consts.DIRENT_TYPE_DOTDOT, ".."),
        Dirent(file_inode.number, consts.DIRENT_TYPE_FILE, source_file.name),
    ]
    uroot_blob: bytes = b"".join(d.to_bytes() for d in uroot_dirents)
    super_root_dirents: list[Dirent] = [
        Dirent(fpt_inode.number, consts.DIRENT_TYPE_FILE, "flat_path_table"),
        Dirent(uroot_inode.number, consts.DIRENT_TYPE_DIRECTORY, "uroot"),
    ]

    inode_count: int = len(inodes)
    inode_size: int = consts.INODE_D32_SIZE
    inodes_per_block: int = block_size // inode_size
    inode_block_count: int = ceil_div(inode_count, inodes_per_block)

    ndblock: int = 1 + inode_block_count
    super_root_inode.db[0] = ndblock
    ndblock += super_root_inode.blocks
    fpt_inode.size = len(fpt_blob)
    fpt_inode.size_compressed = len(fpt_blob)
    fpt_inode.blocks = max(1, ceil_div(len(fpt_blob), block_size))
    fpt_inode.db[0] = ndblock
    for i in range(1, consts.MAX_DIRECT_BLOCKS):
        fpt_inode.db[i] = -1
    ndblock += fpt_inode.blocks
    ndblock += 1
    reserved_empty_blocks: set[int] = {ndblock - 1}
    uroot_inode.blocks = max(1, ceil_div(len(uroot_blob), block_size))
    uroot_inode.size = uroot_inode.blocks * block_size
    uroot_inode.size_compressed = uroot_inode.size
    uroot_inode.db[0] = ndblock
    for i in range(1, consts.MAX_DIRECT_BLOCKS):
        uroot_inode.db[i] = -1
    ndblock += uroot_inode.blocks
    file_inode.db[0] = ndblock
    for i in range(1, consts.MAX_DIRECT_BLOCKS):
        file_inode.db[i] = -1
    payload_base: int = file_inode.db[0] * block_size

    mode: int = compose_pfs_mode_with_options(inode_bits=32, case_insensitive=case_insensitive, signed=False, encrypted=encrypted)
    progress.status("\nPreparing to write PFS image (streaming)... ")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    is_spooled = False
    tmp_path = None
    out = None

    try:
        if use_ram_if_possible:
            from .utils import get_available_ram_bytes
            free_ram = get_available_ram_bytes()
            max_ram_usage = int(free_ram * 0.8)
            if free_ram > 0 and raw_size <= max_ram_usage:
                import tempfile
                out = tempfile.SpooledTemporaryFile(max_size=max_ram_usage, mode="w+b")
                is_spooled = True
                tmp_path = None
                progress.status(f"Writing PFS image to RAM (up to {human_readable_size(max_ram_usage)}) before saving to {output_path}... ")
            else:
                tmp_path = Path(str(output_path) + ".tmp")
                out = open(tmp_path, "w+b", buffering=64 * 1024 * 1024)
                is_spooled = False
                progress.status(f"Writing PFS image to disk (buffered) at {tmp_path}... ")
        else:
            tmp_path = Path(str(output_path) + ".tmp")
            out = open(tmp_path, "w+b", buffering=64 * 1024 * 1024)
            is_spooled = False
            progress.status(f"Writing PFS image to disk at {tmp_path}... ")

        stored_size: int = raw_size
        is_compressed: bool = False
        gain_pct: float = 0.0
        hypothetical_size: int = 0

        out.write(_pack_pfs_header_block(block_size=block_size, pfs_version=pfs_version, mode=mode, nblock=1, inode_count=inode_count, final_ndblock=0, inode_block_count=inode_block_count, now=now, signed=False, encrypted=encrypted, seed=seed))
        out.seek(block_size)
        _write_inode_table(out=out, inodes=inodes, signed=False, signed_inode_bits=32, block_size=block_size, inode_size=inode_size)
        out.seek(super_root_inode.db[0] * block_size)
        for d in super_root_dirents:
            out.write(d.to_bytes())
        out.seek(fpt_inode.db[0] * block_size)
        out.write(fpt_blob)
        out.seek(uroot_inode.db[0] * block_size)
        out.write(uroot_blob)

        total_units: int = max(raw_size, 1)
        processed: int = 0

        def report(delta: int) -> None:
            nonlocal processed
            processed += delta
            progress.step("compress", min(processed, total_units), total_units, bytes_processed=processed)

        if should_compress:
            progress.status(f"\nCompressing 1 file ({human_readable_size(raw_size)}) using {block_workers} CPU core{'s' if block_workers != 1 else ''}... ")
            stored_size, is_compressed, gain_pct, hypothetical_size = _encode_pfsc_into_handle(
                out=out, base_offset=payload_base, source_path=source_file, threshold_gain=threshold_gain,
                min_file_gain=min_file_gain, zlib_level=zlib_level, logical_block_size=consts.PFSC_LOGICAL_BLOCK_SIZE,
                block_worker_count=block_workers, progress_callback=report,
            )

        if not is_compressed:
            if raw_size > 0:
                with source_file.open("rb") as f_in:
                    with mmap.mmap(f_in.fileno(), 0, access=mmap.ACCESS_READ) as mm:
                        out.seek(payload_base)
                        out.write(mm)
            raw_padding_size: int = (file_inode.blocks * block_size) - raw_size
            if raw_padding_size > 0:
                out.seek(payload_base + raw_size)
                out.write(b"\x00" * raw_padding_size)
            stored_size = raw_size

        file_inode.blocks = max(1, ceil_div(stored_size, block_size)) if stored_size > 0 else 1
        if is_compressed:
            file_inode.size = stored_size
            file_inode.size_compressed = ceil_div(raw_size, block_size) * block_size
        else:
            file_inode.size = stored_size
            file_inode.size_compressed = stored_size
        file_inode.flags = consts.INODE_FLAG_READONLY | (consts.INODE_FLAG_COMPRESSED if is_compressed else 0)
        final_ndblock: int = file_inode.db[0] + file_inode.blocks

        validate_d32_ranges(inodes, final_ndblock)

        out.seek(0)
        out.write(_pack_pfs_header_block(block_size=block_size, pfs_version=pfs_version, mode=mode, nblock=1, inode_count=inode_count, final_ndblock=final_ndblock, inode_block_count=inode_block_count, now=now, signed=False, encrypted=encrypted, seed=seed))
        out.seek(block_size)
        _write_inode_table(out=out, inodes=inodes, signed=False, signed_inode_bits=32, block_size=block_size, inode_size=inode_size)
        out.truncate(final_ndblock * block_size)

        if encrypted:
            encrypt_image_filesystem(out, block_size=block_size, total_blocks=final_ndblock, ekpfs=resolved_ekpfs, seed=seed, new_crypt=new_crypt, skip_block_numbers=reserved_empty_blocks)

        if is_spooled:
            out.seek(0)
            validate_image_quick(out, block_size, mode, pfs_version, ekpfs=resolved_ekpfs if encrypted else None, new_crypt=new_crypt)
            out.seek(0)
            with output_path.open("wb") as final_out:
                while True:
                    chunk = out.read(32 * 1024 * 1024)
                    if not chunk:
                        break
                    final_out.write(chunk)
            out.close()
        else:
            out.close()
            validate_image_quick(tmp_path, block_size, mode, pfs_version, ekpfs=resolved_ekpfs if encrypted else None, new_crypt=new_crypt)
            shutil.move(str(tmp_path), str(output_path))

        progress.status(f"Successfully wrote {human_readable_size(final_ndblock * block_size)} image")

    except Exception:
        if out and not out.closed:
            out.close()
        if not is_spooled and tmp_path and tmp_path.exists():
            with suppress(FileNotFoundError):
                tmp_path.unlink()
        raise

    return _single_file_build_stats(
        source_file=source_file, output_path=output_path, raw_size=raw_size, stored_size=stored_size,
        is_compressed=is_compressed, hypothetical_size=hypothetical_size, block_size=block_size,
        gain_pct=gain_pct, elapsed_seconds=time.time() - start, verbose=verbose,
    )

def validate_image_quick(source: "Path | BinaryIO", expected_block_size: int, expected_mode: int, expected_version: int, ekpfs: bytes | None = None, new_crypt: bool = False) -> None:
    if isinstance(source, Path):
        fh = source.open("rb")
        close_fh = True
    else:
        fh = source
        close_fh = False
    try:
        fh.seek(0)
        header: ParsedHeader = parse_image_header(fh)
        inodes: list[ParsedInode] = parse_image_inodes(fh, header, ekpfs=ekpfs, new_crypt=new_crypt)
        if header.version != expected_version or header.magic != consts.PFS_MAGIC:
            raise BuildError("Post-write validation failed: invalid header magic/version")
        if header.block_size != expected_block_size:
            raise BuildError("Post-write validation failed: unexpected block size")
        if header.readonly != 1:
            raise BuildError("Post-write validation failed: header readonly byte is not set")
        if header.mode != expected_mode:
            raise BuildError("Post-write validation failed: unexpected mode flags")
        if header.dinode_count < 3 or header.dinode_block_count < 1:
            raise BuildError("Post-write validation failed: inode table looks invalid")
        signed: bool = (expected_mode & consts.PFS_MODE_SIGNED) != 0
        for inode in inodes:
            if inode.mode & consts.INODE_MODE_ANY_WRITE:
                raise BuildError(f"Post-write validation failed: inode {inode.number} has write bits set (mode=0x{inode.mode:04X})")
            if not signed and (inode.flags & consts.INODE_FLAG_READONLY) == 0:
                raise BuildError(f"Post-write validation failed: inode {inode.number} missing readonly flag (flags=0x{inode.flags:08X})")
    finally:
        if close_fh:
            fh.close()

def prompt_overwrite(output_path: Path) -> bool:
    if not output_path.exists():
        return True
    info(f"Output file already exists: {output_path}", icon_name="file")
    while True:
        response = input("Overwrite? [Y/n] ").strip().lower()
        if response in ["y", "yes", ""]:
            tmp_path = Path(str(output_path) + ".tmp")
            if tmp_path.exists():
                with suppress(OSError):
                    tmp_path.unlink()
            return True
        elif response in ["n", "no"]:
            return False
        else:
            info("Please enter 'y' or 'n'")