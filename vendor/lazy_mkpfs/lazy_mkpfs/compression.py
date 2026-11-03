from __future__ import annotations
import uuid
import zlib as stdlib_zlib
import multiprocessing as mp
import queue
import struct
import subprocess
import sys
from contextlib import suppress
from pathlib import Path
from typing import BinaryIO, Callable, Iterator
from . import consts
from .types import BuildError, FileNode, SupportsIntQueue, PFSCHeader
from .utils import _read_exact, ceil_div, resolve_temp_root
from concurrent.futures import ThreadPoolExecutor
from collections import deque

PFSC_PROGRESS_REPORT_BYTES = consts.PFSC_LOGICAL_BLOCK_SIZE * 16
PFSC_SINGLE_FILE_PARALLEL_MIN_SIZE = 256 * 1024 * 1024

_zlib_module = stdlib_zlib

def _auto_install(package_name: str) -> bool:
    """exFAT Image Builder vendoring patch: runtime pip install is DISABLED.

    The upstream implementation ran `pip install <pkg>` via sys.executable
    when an optional backend (isal / zlib-ng) was missing. Inside the frozen
    app there is no pip and runtime auto-install is forbidden, so this is a
    no-op that logs and reports failure. Callers must treat False as "backend
    unavailable" and fall back to stdlib zlib (which set_zlib_backend does).
    """
    print(f"INFO: optional backend '{package_name}' not bundled; "
          f"auto-install is disabled. Using standard library zlib instead.")
    return False

def set_zlib_backend(backend: str = "zlib") -> None:
    """Set the zlib backend module. Options: 'zlib', 'zlib-ng', 'isa-l'.

    Vendoring patch: an ImportError for an optional native backend NEVER
    triggers pip (see _auto_install). It logs and falls through to the stdlib
    zlib fallback at the bottom. When isal IS bundled, the import succeeds and
    ISA-L is used.
    """
    global _zlib_module

    if backend == "isa-l":
        try:
            from isal import isal_zlib
            _zlib_module = isal_zlib
            return
        except ImportError:
            # No pip. Log and fall through to stdlib zlib below.
            _auto_install("isal")

    elif backend == "zlib-ng":
        try:
            from zlib_ng import zlib_ng
            _zlib_module = zlib_ng
            return
        except ImportError:
            # No pip. Log and fall through to stdlib zlib below.
            _auto_install("zlib-ng")

    print(f"WARNING: Falling back to standard library zlib.")
    _zlib_module = stdlib_zlib

def get_zlib():
    return _zlib_module

# --- Worker Initializer for Fast Linux IPC ---
_worker_progress_queue: SupportsIntQueue | None = None

def _init_compression_worker(q: SupportsIntQueue | None) -> None:
    """Initializer for multiprocessing.Pool to share the fast OS-pipe queue."""
    global _worker_progress_queue
    _worker_progress_queue = q

def should_skip_executable_compression(file_name: str, file_path: str) -> bool:
    lower_name: str = file_name.lower()
    lower_path: str = file_path.lower()
    return (
        (lower_name.startswith("eboot") and lower_name.endswith(".bin"))
        or (lower_name.startswith("param") and lower_name.endswith(".sfx"))
        or lower_name.endswith(".prx")
        or lower_name.endswith(".sprx")
        or lower_name.endswith(".json")
        or lower_name.endswith(".txt")
        or lower_name.endswith(".png")
        or lower_name.endswith("keystone")
        or ("sce_module" in lower_path)
        or ("sce_sys" in lower_path)
    )

def store_file_node_raw(file_node: FileNode) -> None:
    file_node.stored_source_path = file_node.abs_path
    file_node.stored_source_is_temp = False
    file_node.stored_size = file_node.raw_size
    file_node.compressed = False
    file_node.gain_pct = 0.0
    file_node.hypothetical_compressed_size = file_node.raw_size

def _split_pfsc_blocks(payload: bytes, logical_block_size: int) -> list[bytes]:
    if logical_block_size <= 0:
        raise ValueError(f"logical_block_size must be positive, got {logical_block_size}")
    return [payload[offset : offset + logical_block_size] for offset in range(0, len(payload), logical_block_size)]

def _pfsc_header_size(*, block_count: int, logical_block_size: int) -> int:
    if block_count < 0:
        raise ValueError(f"block_count must be non-negative, got {block_count}")
    if logical_block_size <= 0:
        raise ValueError(f"logical_block_size must be positive, got {logical_block_size}")
    pointer_table_size: int = (block_count + 1) * consts.PFSC_OFFSET_ENTRY_SIZE
    extra_table_bytes: int = max(0, pointer_table_size - consts.PFSC_INITIAL_OFFSET_TABLE_CAPACITY)
    extra_blocks: int = ceil_div(extra_table_bytes, logical_block_size) if extra_table_bytes > 0 else 0
    return consts.PFSC_INITIAL_DATA_OFFSET + (extra_blocks * logical_block_size)

def estimate_pfsc_spool_size(*, raw_size: int, logical_block_size: int = consts.PFSC_LOGICAL_BLOCK_SIZE) -> int:
    if raw_size < 0:
        raise ValueError(f"raw_size must be non-negative, got {raw_size}")
    if logical_block_size <= 0:
        raise ValueError(f"logical_block_size must be positive, got {logical_block_size}")
    if raw_size == 0:
        return 0
    block_count: int = ceil_div(raw_size, logical_block_size)
    padded_payload_size: int = block_count * logical_block_size
    return _pfsc_header_size(block_count=block_count, logical_block_size=logical_block_size) + padded_payload_size

def _should_store_pfsc_block_compressed(*, compressed_block_size: int, logical_block_size: int, gain_pct: float, threshold_gain: int) -> bool:
    return compressed_block_size < logical_block_size and gain_pct >= threshold_gain

def encode_pfsc_payload(raw: bytes, threshold_gain: int, zlib_level: int, logical_block_size: int = consts.PFSC_LOGICAL_BLOCK_SIZE, progress_callback: Callable[[int], None] | None = None) -> tuple[bytes, float, int]:
    if not (0 <= threshold_gain <= 100):
        raise ValueError(f"threshold_gain must be between 0 and 100 inclusive, got {threshold_gain}")
    if logical_block_size <= 0:
        raise ValueError(f"logical_block_size must be positive, got {logical_block_size}")

    logical_blocks: list[bytes] = _split_pfsc_blocks(payload=raw, logical_block_size=logical_block_size)
    block_count: int = len(logical_blocks)
    if block_count == 0:
        return b"", 0.0, 0

    encoded_blocks: list[bytes] = []
    all_compressed_size: int = 0
    compressed_blocks: int = 0

    for block in logical_blocks:
        padded_block: bytes = block.ljust(logical_block_size, b"\x00")
        compressed_block: bytes = get_zlib().compress(padded_block, level=zlib_level)
        all_compressed_size += len(compressed_block)
        gain_pct: float = ((len(padded_block) - len(compressed_block)) / len(padded_block)) * 100.0
        store_compressed: bool = _should_store_pfsc_block_compressed(
            compressed_block_size=len(compressed_block), logical_block_size=logical_block_size, gain_pct=gain_pct, threshold_gain=threshold_gain,
        )
        chosen_block: bytes = compressed_block if store_compressed else padded_block
        if store_compressed:
            compressed_blocks += 1
        encoded_blocks.append(chosen_block)
        if progress_callback is not None:
            progress_callback(len(block)) 

    header_size: int = _pfsc_header_size(block_count=block_count, logical_block_size=logical_block_size)
    offsets: list[int] = [header_size]
    for block in encoded_blocks:
        offsets.append(offsets[-1] + len(block))
        
    header: PFSCHeader = PFSCHeader(
        magic=consts.PFSC_MAGIC, unk4=consts.PFSC_UNK4, unk8=consts.PFSC_UNK8,
        logical_block_size=logical_block_size, block_offsets_offset=consts.PFSC_BLOCK_OFFSETS_OFFSET,
        data_offset=header_size, data_length=block_count * logical_block_size,
    )
    header_area: bytearray = bytearray(header_size)
    struct.pack_into("<iiiiqqQq", header_area, 0, header.magic, header.unk4, header.unk8, header.logical_block_size, header.logical_block_size, header.block_offsets_offset, header.data_offset, header.data_length)
    struct.pack_into(f"<{block_count + 1}Q", header_area, consts.PFSC_BLOCK_OFFSETS_OFFSET, *offsets)
    
    encoded_payload: bytes = bytes(header_area) + b"".join(encoded_blocks)
    effective_gain_pct: float = ((len(raw) - len(encoded_payload)) / len(raw)) * 100.0
    hypothetical_all_compressed_size: int = header_size + all_compressed_size
 
    if compressed_blocks == 0 or len(encoded_payload) >= len(raw):
        return raw, 0.0, hypothetical_all_compressed_size

    return encoded_payload, effective_gain_pct, hypothetical_all_compressed_size

def _make_compression_spool_path(*, source_path: Path, temp_folder: Path | None = None) -> Path:
    suffix: str = uuid.uuid4().hex
    safe_name: str = source_path.name.replace(" ", "_")
    temp_root: Path = resolve_temp_root(temp_folder=temp_folder)
    return temp_root / f"mkpfs-{safe_name}.{suffix}.pfsc"

def resolve_block_compression_worker_count(*, requested_cpu_count: int, file_size: int) -> int:
    if requested_cpu_count < 0:
        raise ValueError(f"requested_cpu_count must be non-negative, got {requested_cpu_count}")
    if file_size < 0:
        raise ValueError(f"file_size must be non-negative, got {file_size}")
    if file_size < PFSC_SINGLE_FILE_PARALLEL_MIN_SIZE:
        return 1
    return max(1, requested_cpu_count)

def _iter_pfsc_block_worker_args(*, abs_path: Path, block_count: int, logical_block_size: int, zlib_level: int) -> Iterator[tuple[Path, int, int, int]]:
    for block_index in range(block_count):
        block_offset: int = block_index * logical_block_size
        yield abs_path, block_offset, logical_block_size, zlib_level

def _compress_pfsc_block_lengths_worker(args: tuple[Path, int, int, int]) -> tuple[int, int]:
    abs_path, block_offset, logical_block_size, zlib_level = args
    with abs_path.open("rb") as source_file:
        source_file.seek(block_offset)
        raw_chunk = source_file.read(logical_block_size)
    padded_chunk: bytes = raw_chunk.ljust(logical_block_size, b"\x00")
    compressed_chunk: bytes = get_zlib().compress(padded_chunk, level=zlib_level)
    return len(raw_chunk), len(compressed_chunk)

def _compress_pfsc_block_payload_worker(args: tuple[Path, int, int, int]) -> tuple[bytes, bytes]:
    abs_path, block_offset, logical_block_size, zlib_level = args
    with abs_path.open("rb") as source_file:
        source_file.seek(block_offset)
        raw_chunk = source_file.read(logical_block_size)
    padded_chunk: bytes = raw_chunk.ljust(logical_block_size, b"\x00")
    compressed_chunk: bytes = get_zlib().compress(padded_chunk, level=zlib_level)
    return raw_chunk, compressed_chunk

def _analyze_pfsc_file_storage(*, abs_path: Path, threshold_gain: int, min_file_gain: int, zlib_level: int, logical_block_size: int, block_worker_count: int = 1, progress_callback: Callable[[int], None] | None = None) -> tuple[int, bool, float, int]:
    raw_size: int = abs_path.stat().st_size
    if not (0 <= min_file_gain <= 100):
        raise ValueError(f"min_file_gain must be between 0 and 100 inclusive, got {min_file_gain}")
    if raw_size == 0:
        return 0, False, 0.0, 0

    block_count: int = ceil_div(raw_size, logical_block_size)
    header_size: int = _pfsc_header_size(block_count=block_count, logical_block_size=logical_block_size)
    chosen_payload_size: int = 0
    all_compressed_size: int = 0
    compressed_blocks: int = 0
    effective_block_workers: int = max(1, min(block_worker_count, block_count))

    if effective_block_workers == 1:
        with abs_path.open("rb") as source_file:
            for _idx in range(block_count):
                chunk: bytes = source_file.read(logical_block_size)
                padded_chunk: bytes = chunk.ljust(logical_block_size, b"\x00")
                compressed_chunk: bytes = get_zlib().compress(padded_chunk, level=zlib_level)
                all_compressed_size += len(compressed_chunk)
                gain_pct: float = ((len(padded_chunk) - len(compressed_chunk)) / len(padded_chunk)) * 100.0
                if _should_store_pfsc_block_compressed(compressed_block_size=len(compressed_chunk), logical_block_size=logical_block_size, gain_pct=gain_pct, threshold_gain=threshold_gain):
                    chosen_payload_size += len(compressed_chunk)
                    compressed_blocks += 1
                else:
                    chosen_payload_size += len(padded_chunk)
                if progress_callback is not None:
                    progress_callback(len(chunk))
    else:
        worker_args_iter: Iterator[tuple[Path, int, int, int]] = _iter_pfsc_block_worker_args(abs_path=abs_path, block_count=block_count, logical_block_size=logical_block_size, zlib_level=zlib_level)
        with ThreadPoolExecutor(max_workers=effective_block_workers) as executor:
            max_in_flight = effective_block_workers * 8
            it = iter(worker_args_iter)
            futures = deque()
            def bounded_gen():
                for args in it:
                    if len(futures) >= max_in_flight:
                        yield futures.popleft().result()
                    futures.append(executor.submit(_compress_pfsc_block_lengths_worker, args))
                while futures:
                    yield futures.popleft().result()

            results_iter = bounded_gen()
            for raw_block_len, compressed_block_len in results_iter:
                all_compressed_size += compressed_block_len
                padded_block_len: int = logical_block_size
                gain_pct: float = ((padded_block_len - compressed_block_len) / padded_block_len) * 100.0
                if _should_store_pfsc_block_compressed(compressed_block_size=compressed_block_len, logical_block_size=logical_block_size, gain_pct=gain_pct, threshold_gain=threshold_gain):
                    chosen_payload_size += compressed_block_len
                    compressed_blocks += 1
                else:
                    chosen_payload_size += padded_block_len
                if progress_callback is not None:
                    progress_callback(raw_block_len)

    encoded_payload_size: int = header_size + chosen_payload_size
    hypothetical_all_compressed_size: int = header_size + all_compressed_size
    if compressed_blocks == 0 or encoded_payload_size >= raw_size:
        return raw_size, False, 0.0, hypothetical_all_compressed_size
    effective_gain_pct: float = ((raw_size - encoded_payload_size) / raw_size) * 100.0
    if effective_gain_pct < min_file_gain:
        return raw_size, False, effective_gain_pct, hypothetical_all_compressed_size
    return encoded_payload_size, True, effective_gain_pct, hypothetical_all_compressed_size

def _encode_pfsc_into_handle(*, out: BinaryIO, base_offset: int, source_path: Path, threshold_gain: int, min_file_gain: int, zlib_level: int, logical_block_size: int, block_worker_count: int = 1, progress_callback: Callable[[int], None] | None = None) -> tuple[int, bool, float, int]:
    raw_size: int = source_path.stat().st_size
    if not (0 <= min_file_gain <= 100):
        raise ValueError(f"min_file_gain must be between 0 and 100 inclusive, got {min_file_gain}")
    if raw_size == 0:
        return 0, False, 0.0, 0

    block_count: int = ceil_div(raw_size, logical_block_size)
    header_size: int = _pfsc_header_size(block_count=block_count, logical_block_size=logical_block_size)
    offsets: list[int] = [header_size]
    all_compressed_size: int = 0
    compressed_blocks: int = 0
    effective_block_workers: int = max(1, min(block_worker_count, block_count))

    out.seek(base_offset + header_size)
    if effective_block_workers == 1:
        with source_path.open("rb") as source_file:
            for _idx in range(block_count):
                chunk: bytes = source_file.read(logical_block_size)
                padded_chunk: bytes = chunk.ljust(logical_block_size, b"\x00")
                compressed_chunk: bytes = get_zlib().compress(padded_chunk, level=zlib_level)
                all_compressed_size += len(compressed_chunk)
                gain_pct: float = ((len(padded_chunk) - len(compressed_chunk)) / len(padded_chunk)) * 100.0
                store_compressed: bool = _should_store_pfsc_block_compressed(compressed_block_size=len(compressed_chunk), logical_block_size=logical_block_size, gain_pct=gain_pct, threshold_gain=threshold_gain)
                selected_chunk: bytes = compressed_chunk if store_compressed else padded_chunk
                if store_compressed:
                    compressed_blocks += 1
                out.write(selected_chunk)
                offsets.append(offsets[-1] + len(selected_chunk))
                if progress_callback is not None:
                    progress_callback(len(chunk))
    else:  
        worker_args_iter: Iterator[tuple[Path, int, int, int]] = _iter_pfsc_block_worker_args(abs_path=source_path, block_count=block_count, logical_block_size=logical_block_size, zlib_level=zlib_level)
        with ThreadPoolExecutor(max_workers=effective_block_workers) as executor:
            max_in_flight = effective_block_workers * 8
            it = iter(worker_args_iter)
            futures = deque()
            def bounded_gen():
                for args in it:
                    if len(futures) >= max_in_flight:
                        yield futures.popleft().result()
                    futures.append(executor.submit(_compress_pfsc_block_payload_worker, args))
                while futures:
                    yield futures.popleft().result()

            results_iter = bounded_gen()
            for raw_chunk, compressed_chunk in results_iter:
                padded_chunk: bytes = raw_chunk.ljust(logical_block_size, b"\x00")
                all_compressed_size += len(compressed_chunk)
                gain_pct: float = ((len(padded_chunk) - len(compressed_chunk)) / len(padded_chunk)) * 100.0
                store_compressed: bool = _should_store_pfsc_block_compressed(compressed_block_size=len(compressed_chunk), logical_block_size=logical_block_size, gain_pct=gain_pct, threshold_gain=threshold_gain)
                selected_chunk: bytes = compressed_chunk if store_compressed else padded_chunk 
                if store_compressed:
                    compressed_blocks += 1
                out.write(selected_chunk)
                offsets.append(offsets[-1] + len(selected_chunk))
                if progress_callback is not None:
                    progress_callback(len(raw_chunk))

    encoded_payload_size: int = offsets[-1]
    hypothetical_all_compressed_size: int = header_size + all_compressed_size
    if compressed_blocks == 0 or encoded_payload_size >= raw_size:
        return raw_size, False, 0.0, hypothetical_all_compressed_size

    effective_gain_pct: float = ((raw_size - encoded_payload_size) / raw_size) * 100.0
    if effective_gain_pct < min_file_gain:
        return raw_size, False, effective_gain_pct, hypothetical_all_compressed_size

    header: PFSCHeader = PFSCHeader(magic=consts.PFSC_MAGIC, unk4=consts.PFSC_UNK4, unk8=consts.PFSC_UNK8, logical_block_size=logical_block_size, block_offsets_offset=consts.PFSC_BLOCK_OFFSETS_OFFSET, data_offset=header_size, data_length=block_count * logical_block_size)
    header_area: bytearray = bytearray(header_size)
    struct.pack_into("<iiiiqqQq", header_area, 0, header.magic, header.unk4, header.unk8, header.logical_block_size, header.logical_block_size, header.block_offsets_offset, header.data_offset, header.data_length)
    struct.pack_into(f"<{block_count + 1}Q", header_area, consts.PFSC_BLOCK_OFFSETS_OFFSET, *offsets)
    out.seek(base_offset)
    out.write(header_area)
    return encoded_payload_size, True, effective_gain_pct, hypothetical_all_compressed_size

def _encode_pfsc_file_to_spool(*, abs_path: Path, spool_path: Path, threshold_gain: int, min_file_gain: int, zlib_level: int, logical_block_size: int, block_worker_count: int = 1, progress_callback: Callable[[int], None] | None = None) -> tuple[int, bool, float, int]:
    try:
        with spool_path.open("w+b") as spool_file:
            stored_size, is_compressed, gain_pct, hypothetical_all_compressed_size = _encode_pfsc_into_handle(
                out=spool_file, base_offset=0, source_path=abs_path, threshold_gain=threshold_gain, min_file_gain=min_file_gain, zlib_level=zlib_level, logical_block_size=logical_block_size, block_worker_count=block_worker_count, progress_callback=progress_callback,
            )
            spool_file.truncate(stored_size)
    except OSError:
        with suppress(OSError):
            spool_path.unlink()
        raise
    return stored_size, is_compressed, gain_pct, hypothetical_all_compressed_size

def write_source_to_blocks(out: BinaryIO, source_path: Path, payload_size: int, blocks: list[int], block_size: int, progress_callback: Callable[[int], None] | None = None) -> None:
    if payload_size <= 0: return
    chunk_size: int = min(block_size, 1024 * 1024)
    with source_path.open("rb") as source_file:
        remaining_bytes: int = payload_size
        for block in blocks:
            if remaining_bytes <= 0: break
            out.seek(block * block_size)
            block_bytes_to_copy: int = min(block_size, remaining_bytes)
            while block_bytes_to_copy > 0:
                current_chunk_size = min(chunk_size, block_bytes_to_copy)
                chunk = source_file.read(current_chunk_size)
                if not chunk: raise BuildError("Stored payload source ended before expected size")
                out.write(chunk)
                block_bytes_to_copy -= len(chunk)
                remaining_bytes -= len(chunk)
                if progress_callback is not None: progress_callback(len(chunk))

def write_source_to_offset(out: BinaryIO, source_path: Path, payload_size: int, offset: int, progress_callback: Callable[[int], None] | None = None) -> None:
    if payload_size <= 0: return
    out.seek(offset)
    chunk_size: int = 1024 * 1024
    with source_path.open("rb") as source_file:
        remaining = payload_size
        while remaining > 0:
            to_read = min(chunk_size, remaining)
            chunk = source_file.read(to_read)
            if not chunk: raise BuildError("Stored payload source ended before expected size")
            out.write(chunk)
            remaining -= len(chunk)
            if progress_callback is not None: progress_callback(len(chunk))

def _drain_compression_progress_queue(progress_queue: SupportsIntQueue) -> int:
    drained_bytes: int = 0
    while True:
        try: drained_bytes += progress_queue.get_nowait()
        except queue.Empty: break
    return drained_bytes

def resolve_compression_worker_count(*, requested_cpu_count: int) -> int:
    if requested_cpu_count < 0: raise ValueError(f"requested_cpu_count must be non-negative, got {requested_cpu_count}")
    resolved_count: int = max(1, mp.cpu_count() - 1) if requested_cpu_count == 0 else requested_cpu_count
    return max(1, resolved_count)

def _compress_files_in_process(*, file_nodes_sorted: list[FileNode], threshold_gain: int, min_file_gain: int, min_compress_size: int, zlib_level: int, compression_cpu_count: int, dry_run: bool, total_bytes_to_process: int, progress, temp_folder: Path | None) -> None:
    progress_total_units: int = total_bytes_to_process if total_bytes_to_process > 0 else len(file_nodes_sorted)
    displayed_progress_units: int = 0
    processed_raw_bytes: int = 0
    progress.step("compress", 0, progress_total_units, bytes_processed=0)

    for completed_files, file_node in enumerate(file_nodes_sorted, start=1):
        file_progress_bytes: int = 0
        file_base_processed_bytes: int = processed_raw_bytes

        def report_progress(delta_bytes: int, *, _file_base_processed_bytes: int = file_base_processed_bytes, _completed_files: int = completed_files) -> None:
            nonlocal displayed_progress_units, file_progress_bytes
            file_progress_bytes += delta_bytes
            target_units: int = min(total_bytes_to_process, _file_base_processed_bytes + file_progress_bytes) if total_bytes_to_process > 0 else _completed_files
            if target_units <= displayed_progress_units: return
            displayed_progress_units = target_units
            progress.step("compress", displayed_progress_units, progress_total_units, bytes_processed=displayed_progress_units if total_bytes_to_process > 0 else 0)

        if file_node.raw_size == 0 or file_node.raw_size < min_compress_size:
            store_file_node_raw(file_node)
        else:
            block_worker_count: int = resolve_block_compression_worker_count(requested_cpu_count=compression_cpu_count, file_size=file_node.raw_size)
            if dry_run:
                stored_size, is_compressed, gain_pct, hypothetical_compressed_size = _analyze_pfsc_file_storage(abs_path=file_node.abs_path, threshold_gain=threshold_gain, min_file_gain=min_file_gain, zlib_level=zlib_level, logical_block_size=consts.PFSC_LOGICAL_BLOCK_SIZE, block_worker_count=block_worker_count, progress_callback=report_progress)
                file_node.stored_source_path = file_node.abs_path
                file_node.stored_source_is_temp = False
                file_node.stored_size = stored_size
                file_node.compressed = is_compressed
                file_node.gain_pct = gain_pct
                file_node.hypothetical_compressed_size = hypothetical_compressed_size
            else:
                spool_path: Path = _make_compression_spool_path(source_path=file_node.abs_path, temp_folder=temp_folder)
                stored_size, is_compressed, gain_pct, hypothetical_compressed_size = _encode_pfsc_file_to_spool(abs_path=file_node.abs_path, spool_path=spool_path, threshold_gain=threshold_gain, min_file_gain=min_file_gain, zlib_level=zlib_level, logical_block_size=consts.PFSC_LOGICAL_BLOCK_SIZE, block_worker_count=block_worker_count, progress_callback=report_progress)
                if is_compressed:
                    file_node.stored_source_path = spool_path
                    file_node.stored_source_is_temp = True
                else:
                    with suppress(FileNotFoundError): spool_path.unlink()
                    file_node.stored_source_path = file_node.abs_path
                    file_node.stored_source_is_temp = False
                file_node.stored_size = stored_size
                file_node.compressed = is_compressed
                file_node.gain_pct = gain_pct
                file_node.hypothetical_compressed_size = hypothetical_compressed_size

        processed_raw_bytes += file_node.raw_size
        target_units = processed_raw_bytes if total_bytes_to_process > 0 else completed_files
        if target_units > displayed_progress_units:
            displayed_progress_units = target_units
            progress.step("compress", displayed_progress_units, progress_total_units, bytes_processed=displayed_progress_units if total_bytes_to_process > 0 else 0)

    if displayed_progress_units < progress_total_units:
        progress.step("compress", progress_total_units, progress_total_units, bytes_processed=total_bytes_to_process if total_bytes_to_process > 0 else 0)

def cleanup_temporary_file_node_payloads(*, file_nodes: list[FileNode]) -> None:
    for file_node in file_nodes:
        if not file_node.stored_source_is_temp or file_node.stored_source_path is None: continue
        with suppress(OSError): file_node.stored_source_path.unlink()
        file_node.stored_source_path = None
        file_node.stored_source_is_temp = False

def compute_file_storage(file_node: FileNode, compress: bool, threshold_gain: int, min_file_gain: int = 0, min_compress_size: int = 0, block_size: int = consts.PFSC_LOGICAL_BLOCK_SIZE, zlib_level: int = 7) -> None:
    if not (0 <= threshold_gain <= 100): raise ValueError(f"threshold_gain must be between 0 and 100 inclusive, got {threshold_gain}")
    raw_size: int = file_node.abs_path.stat().st_size
    if not compress or raw_size == 0 or raw_size < min_compress_size:
        file_node.stored_source_path = file_node.abs_path
        file_node.stored_source_is_temp = False
        file_node.stored_size = raw_size
        file_node.compressed = False
        file_node.gain_pct = 0.0
        file_node.hypothetical_compressed_size = 0
        return

    stored_size, is_compressed, gain_pct, hypothetical_size = _analyze_pfsc_file_storage(abs_path=file_node.abs_path, threshold_gain=threshold_gain, min_file_gain=min_file_gain, zlib_level=zlib_level, logical_block_size=block_size, block_worker_count=1)
    file_node.stored_source_path = file_node.abs_path
    file_node.stored_source_is_temp = False
    file_node.stored_size = stored_size
    file_node.compressed = is_compressed
    file_node.gain_pct = gain_pct
    file_node.hypothetical_compressed_size = hypothetical_size

# --- UPDATED WORKER SIGNATURE (Removed progress_queue from args tuple) ---
def _compute_file_storage_worker(
    args: tuple[Path, int, int, int, bool, int, int, bool, Path | None],
) -> tuple[Path, Path, bool, int, bool, float, int]:
    (abs_path, threshold_gain, min_file_gain, min_compress_size, compress, _block_size, zlib_level, dry_run, temp_folder) = args
    
    # Fetch queue from module-level global set by Pool initializer
    progress_queue = _worker_progress_queue

    raw_size: int = abs_path.stat().st_size
    if not compress or raw_size == 0 or raw_size < min_compress_size:
        return abs_path, abs_path, False, raw_size, False, 0.0, 0

    batched_progress_bytes: int = 0
    def report_progress(delta_bytes: int) -> None:
        nonlocal batched_progress_bytes
        batched_progress_bytes += delta_bytes
        if progress_queue is not None and batched_progress_bytes >= PFSC_PROGRESS_REPORT_BYTES:
            progress_queue.put(batched_progress_bytes)
            batched_progress_bytes = 0

    if dry_run:
        stored_size, is_compressed, gain_pct, hypothetical_compressed_size = _analyze_pfsc_file_storage(abs_path=abs_path, threshold_gain=threshold_gain, min_file_gain=min_file_gain, zlib_level=zlib_level, logical_block_size=consts.PFSC_LOGICAL_BLOCK_SIZE, block_worker_count=1, progress_callback=report_progress if progress_queue is not None else None)
        stored_source_path: Path = abs_path
        stored_source_is_temp: bool = False
    else:
        spool_path: Path = _make_compression_spool_path(source_path=abs_path, temp_folder=temp_folder)
        stored_size, is_compressed, gain_pct, hypothetical_compressed_size = _encode_pfsc_file_to_spool(abs_path=abs_path, spool_path=spool_path, threshold_gain=threshold_gain, min_file_gain=min_file_gain, zlib_level=zlib_level, logical_block_size=consts.PFSC_LOGICAL_BLOCK_SIZE, block_worker_count=1, progress_callback=report_progress if progress_queue is not None else None)
        if is_compressed:
            stored_source_path = spool_path
            stored_source_is_temp = True
        else:
            with suppress(FileNotFoundError): spool_path.unlink()
            stored_source_path = abs_path
            stored_source_is_temp = False
            
    if progress_queue is not None and batched_progress_bytes > 0:
        progress_queue.put(batched_progress_bytes)
        
    return (abs_path, stored_source_path, stored_source_is_temp, stored_size, is_compressed, gain_pct, hypothetical_compressed_size)

def _parse_pfsc_header(head: bytes) -> tuple[int, int, int, int, int]:
    """Parse and validate the fixed PFSC header fields."""
    if len(head) < consts.PFSC_HEADER_SIZE:
        raise ValueError("PFSC payload is too small for header")
    (
        magic,
        unk4,
        unk8,
        logical_block_size,
        logical_block_size_2,
        block_offsets_offset,
        data_offset,
        logical_size,
    ) = struct.unpack_from("<iiiiqqQq", head, 0)

    if magic != consts.PFSC_MAGIC:
        raise ValueError(f"invalid PFSC magic 0x{magic:08X}")
    if unk4 != consts.PFSC_UNK4:
        raise ValueError(f"invalid PFSC unk4 value {unk4}, expected {consts.PFSC_UNK4}")
    if unk8 != consts.PFSC_UNK8:
        raise ValueError(f"invalid PFSC unk8 value {unk8}, expected {consts.PFSC_UNK8}")
    if logical_block_size != consts.PFSC_LOGICAL_BLOCK_SIZE:
        raise ValueError(
            f"invalid PFSC logical block size {logical_block_size}, expected {consts.PFSC_LOGICAL_BLOCK_SIZE}"
        )
    if logical_block_size_2 != logical_block_size:
        raise ValueError("PFSC block size mismatch between block_sz and block_sz2")
    if logical_size < 0:
        raise ValueError("PFSC logical size is negative")
    if logical_size % logical_block_size != 0:
        raise ValueError("PFSC logical size is not aligned to the logical block size")
    if block_offsets_offset < consts.PFSC_HEADER_SIZE:
        raise ValueError("PFSC block offset table overlaps header")
    if block_offsets_offset != consts.PFSC_BLOCK_OFFSETS_OFFSET:
        raise ValueError(
            f"invalid PFSC block offset table pointer {block_offsets_offset}, "
            f"expected {consts.PFSC_BLOCK_OFFSETS_OFFSET}"
        )
    if data_offset < consts.PFSC_INITIAL_DATA_OFFSET:
        raise ValueError("PFSC data offset is smaller than the minimum compatible header span")

    block_count: int = logical_size // logical_block_size
    return logical_block_size, block_count, block_offsets_offset, data_offset, logical_size

def _decode_pfsc_block(stored_block: bytes, logical_block_size: int, idx: int) -> bytes:
    """Decode one stored PFSC block to its logical bytes."""
    if len(stored_block) == logical_block_size:
        return stored_block
    if len(stored_block) < logical_block_size:
        try:
            # Use stdlib_zlib.error since we imported zlib as stdlib_zlib
            logical_block: bytes = get_zlib().decompress(stored_block)
        except stdlib_zlib.error as exc:
            raise ValueError(f"PFSC block {idx} failed to decompress: {exc}") from exc
        if len(logical_block) != logical_block_size:
            raise ValueError(
                f"PFSC block {idx} decompressed to {len(logical_block)} bytes, expected {logical_block_size}"
            )
        return logical_block
    raise ValueError(f"PFSC block {idx} stored size {len(stored_block)} exceeds logical size {logical_block_size}")

def decode_pfsc_payload(payload: bytes, expected_logical_size: int | None = None) -> bytes:
    """Decode PFSC block-compressed payload bytes."""
    logical_block_size, block_count, block_offsets_offset, data_offset, logical_size = _parse_pfsc_header(payload)
    if data_offset > len(payload):
        raise ValueError("PFSC data offset exceeds payload length")

    offsets_size: int = (block_count + 1) * consts.PFSC_OFFSET_ENTRY_SIZE
    offsets_end: int = block_offsets_offset + offsets_size
    if offsets_end > data_offset or offsets_end > len(payload):
        raise ValueError("PFSC payload is truncated before block offset table")

    offsets: list[int] = list(struct.unpack_from(f"<{block_count + 1}Q", payload, block_offsets_offset))
    if offsets[0] != data_offset:
        raise ValueError("PFSC block offsets must start at data_start")
    if offsets[-1] > len(payload):
        raise ValueError("PFSC block offsets exceed payload size")
    for idx in range(1, len(offsets)):
        if offsets[idx] < offsets[idx - 1]:
            raise ValueError("PFSC block offsets are not monotonic")

    logical_out: bytearray = bytearray()
    for idx in range(block_count):
        stored_block: bytes = payload[offsets[idx] : offsets[idx + 1]]
        logical_out.extend(_decode_pfsc_block(stored_block, logical_block_size, idx))

    logical_payload: bytes = bytes(logical_out)
    if len(logical_payload) != logical_size:
        raise ValueError(f"PFSC logical output size {len(logical_payload)} does not match header size {logical_size}")
    if expected_logical_size is not None:
        if expected_logical_size < 0:
            raise ValueError("expected inode logical size is negative")
        if expected_logical_size > logical_size:
            raise ValueError(f"PFSC logical size {logical_size} is smaller than inode size {expected_logical_size}")
        return logical_payload[:expected_logical_size]
    return logical_payload

def decode_inode_payload(
    payload: bytes,
    inode,  # Type hint omitted to avoid NameError if ParsedInode isn't explicitly imported here
) -> bytes:
    """Decode one inode payload to logical bytes."""
    if not inode.is_compressed:
        return payload
    return decode_pfsc_payload(payload=payload, expected_logical_size=inode.logical_size)