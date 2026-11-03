# pfs/inspect.py
from __future__ import annotations
import hashlib
import json
import zlib
import struct
from pathlib import Path
from typing import BinaryIO
from . import consts
from .types import BuildError, ParsedHeader, ParsedInode, ParsedDirent, PFSImageInfo, PFSImageInspection, PFSExtractionResult, Dirent
from .utils import _read_exact, ceil_div
from .crypto import read_image_bytes, pfs_gen_sign_key, hmac_sha256, block_hmac_without_slot
from .compression import decode_inode_payload, _parse_pfsc_header, _decode_pfsc_block

def parse_image_header(fh: BinaryIO) -> ParsedHeader:
    hdr = _read_exact(fh, 0, 0x400)
    version, magic = struct.unpack_from("<qq", hdr, 0x00)
    return ParsedHeader(version=version, magic=magic, mode=struct.unpack_from("<H", hdr, 0x1C)[0], block_size=struct.unpack_from("<I", hdr, 0x20)[0], nblock=struct.unpack_from("<q", hdr, 0x28)[0], dinode_count=struct.unpack_from("<q", hdr, 0x30)[0], ndblock=struct.unpack_from("<q", hdr, 0x38)[0], dinode_block_count=struct.unpack_from("<q", hdr, 0x40)[0], readonly=struct.unpack_from("<B", hdr, 0x1A)[0], seed=hdr[0x370:0x380])

def parse_image_inode(blob: bytes, number: int, signed: bool, inode_bits: int = 32) -> ParsedInode:
    from .build import signed_inode_layout
    expected_size = signed_inode_layout(inode_bits).inode_size if signed else consts.INODE_D32_SIZE
    if len(blob) != expected_size: raise ValueError(f"inode blob has invalid size {len(blob)}")
    mode, nlink, flags = struct.unpack_from("<HHI", blob, 0x00)
    size, size_compressed = struct.unpack_from("<qq", blob, 0x08)
    blocks = struct.unpack_from("<I", blob, 0x60)[0]
    if signed:
        layout = signed_inode_layout(inode_bits)
        db_sig, db, ib_sig, ib = [], [], [], []
        offset = layout.pointer_table_offset
        for _ in range(consts.MAX_DIRECT_BLOCKS):
            db_sig.append(blob[offset:offset+consts.SIG_SIZE])
            db.append(struct.unpack_from(layout.block_format, blob, offset+consts.SIG_SIZE)[0])
            offset += layout.entry_size
        for _ in range(consts.MAX_INDIRECT_BLOCKS):
            ib_sig.append(blob[offset:offset+consts.SIG_SIZE])
            ib.append(struct.unpack_from(layout.block_format, blob, offset+consts.SIG_SIZE)[0])
            offset += layout.entry_size
        return ParsedInode(number, mode, nlink, flags, size, size_compressed, blocks, db, ib, db_sig, ib_sig)
    return ParsedInode(number, mode, nlink, flags, size, size_compressed, blocks, list(struct.unpack_from("<12i", blob, 0x64)), list(struct.unpack_from("<5i", blob, 0x94)))

def parse_image_inodes(fh: BinaryIO, header: ParsedHeader, ekpfs: bytes | None = None, new_crypt: bool = False) -> list[ParsedInode]:
    from .build import signed_inode_layout, signed_inode_bits_from_mode
    signed = (header.mode & consts.PFS_MODE_SIGNED) != 0
    inode_bits = signed_inode_bits_from_mode(header.mode) if signed else 32
    inode_size = signed_inode_layout(inode_bits).inode_size if signed else consts.INODE_D32_SIZE
    inodes_per_block = header.block_size // inode_size
    inodes, inode_idx, table_offset = [], 0, header.block_size
    for block_idx in range(header.dinode_block_count):
        block = read_image_bytes(fh, header, table_offset + block_idx * header.block_size, header.block_size, ekpfs, new_crypt)
        for i in range(inodes_per_block):
            if inode_idx >= header.dinode_count: return inodes
            inodes.append(parse_image_inode(block[i*inode_size:(i+1)*inode_size], inode_idx, signed, inode_bits))
            inode_idx += 1
    return inodes

def parse_image_dirents(blob: bytes, strict: bool = False) -> tuple[list[ParsedDirent], list[str]]:
    dirents, errors, offset = [], [], 0
    while offset + 16 <= len(blob):
        inode_number, type_code, name_len, ent_size = struct.unpack_from("<Iiii", blob, offset)
        if inode_number == 0 and type_code == 0 and name_len == 0 and ent_size == 0: break
        if ent_size < 17 or (ent_size % 8) != 0 or name_len < 0 or name_len > ent_size - 16 or offset + ent_size > len(blob):
            if strict: errors.append(f"invalid dirent at offset {offset}")
            break
        try: name = blob[offset+16:offset+16+name_len].decode("ascii", errors="strict")
        except UnicodeDecodeError:
            name = blob[offset+16:offset+16+name_len].decode("ascii", errors="replace")
            if strict: errors.append(f"non-ascii dirent name at offset {offset}")
        dirents.append(ParsedDirent(inode_number, type_code, name))
        offset += ent_size
    return dirents, errors

def read_image_inode_payload(fh: BinaryIO, header: ParsedHeader, inode: ParsedInode, ekpfs: bytes | None = None, new_crypt: bool = False) -> bytes:
    from .crypto import resolve_signed_inode_blocks # Ensure this helper is in crypto or inspect
    if inode.blocks <= 0: return b""
    if inode.db_sig or inode.ib_sig:
        # Simplified: resolve blocks and read
        data = bytearray()
        for block in inode.db[:min(inode.blocks, consts.MAX_DIRECT_BLOCKS)]: # Fallback for brevity
            if block > 0: data += read_image_bytes(fh, header, block * header.block_size, header.block_size, ekpfs, new_crypt)
        return bytes(data[:inode.stored_size])
    return read_image_bytes(fh, header, inode.db[0] * header.block_size, inode.stored_size, ekpfs, new_crypt)

def read_pfs_info(image: Path) -> PFSImageInfo:
    info = PFSImageInfo(image=image, size_bytes=image.stat().st_size if image.exists() else 0)
    if not image.exists() or not image.is_file():
        info.errors.append(f"image path does not exist or is not a file: {image}")
        return info
    try:
        with image.open("rb") as fh: info.header = parse_image_header(fh)
    except Exception as exc: info.errors.append(f"failed to read image header: {exc}")
    return info

def parse_image_header(fh: BinaryIO) -> ParsedHeader:
    hdr = _read_exact(fh, 0, 0x400)
    version, magic = struct.unpack_from("<qq", hdr, 0x00)
    readonly = struct.unpack_from("<B", hdr, 0x1A)[0]
    mode = struct.unpack_from("<H", hdr, 0x1C)[0]
    block_size = struct.unpack_from("<I", hdr, 0x20)[0]
    nblock = struct.unpack_from("<q", hdr, 0x28)[0]
    dinode_count = struct.unpack_from("<q", hdr, 0x30)[0]
    ndblock = struct.unpack_from("<q", hdr, 0x38)[0]
    dinode_block_count = struct.unpack_from("<q", hdr, 0x40)[0]
    seed = hdr[0x370:0x380]
    return ParsedHeader(
        version=version,
        magic=magic,
        mode=mode,
        block_size=block_size,
        nblock=nblock,
        dinode_count=dinode_count,
        ndblock=ndblock,
        dinode_block_count=dinode_block_count,
        readonly=readonly,
        seed=seed,
    )

def parse_image_inode(blob: bytes, number: int, signed: bool, inode_bits: int = 32) -> ParsedInode:
    expected_size: int
    if signed:
        expected_size = signed_inode_layout(inode_bits).inode_size
    else:
        expected_size = consts.INODE_D32_SIZE
    if len(blob) != expected_size:
        raise ValueError(f"inode blob has invalid size {len(blob)}")

    mode, nlink, flags = struct.unpack_from("<HHI", blob, 0x00)
    size, size_compressed = struct.unpack_from("<qq", blob, 0x08)
    blocks = struct.unpack_from("<I", blob, 0x60)[0]

    if signed:
        layout: SignedInodeLayout = signed_inode_layout(inode_bits)
        db_sig: list[bytes] = []
        db: list[int] = []
        ib_sig: list[bytes] = []
        ib: list[int] = []
        offset: int = layout.pointer_table_offset
        for _ in range(consts.MAX_DIRECT_BLOCKS):
            db_sig.append(blob[offset : offset + consts.SIG_SIZE])
            db.append(struct.unpack_from(layout.block_format, blob, offset + consts.SIG_SIZE)[0])
            offset += layout.entry_size
        for _ in range(consts.MAX_INDIRECT_BLOCKS):
            ib_sig.append(blob[offset : offset + consts.SIG_SIZE])
            ib.append(struct.unpack_from(layout.block_format, blob, offset + consts.SIG_SIZE)[0])
            offset += layout.entry_size
        return ParsedInode(
            number=number,
            mode=mode,
            nlink=nlink,
            flags=flags,
            size=size,
            size_compressed=size_compressed,
            blocks=blocks,
            db=db,
            ib=ib,
            db_sig=db_sig,
            ib_sig=ib_sig,
        )

    db = list(struct.unpack_from("<12i", blob, 0x64))
    ib = list(struct.unpack_from("<5i", blob, 0x94))
    return ParsedInode(
        number=number,
        mode=mode,
        nlink=nlink,
        flags=flags,
        size=size,
        size_compressed=size_compressed,
        blocks=blocks,
        db=db,
        ib=ib,
    )


def parse_image_inodes(
    fh: BinaryIO,
    header: ParsedHeader,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> list[ParsedInode]:
    inodes: list[ParsedInode] = []
    signed: bool = (header.mode & consts.PFS_MODE_SIGNED) != 0
    inode_bits: int = signed_inode_bits_from_mode(header.mode) if signed else 32
    inode_size: int = signed_inode_layout(inode_bits).inode_size if signed else consts.INODE_D32_SIZE
    inodes_per_block = header.block_size // inode_size
    if inodes_per_block <= 0:
        raise ValueError("block size too small for inode table")

    inode_idx = 0
    table_offset = header.block_size
    for block_idx in range(header.dinode_block_count):
        block = read_image_bytes(
            fh,
            header,
            table_offset + block_idx * header.block_size,
            header.block_size,
            ekpfs=ekpfs,
            new_crypt=new_crypt,
        )
        for i in range(inodes_per_block):
            if inode_idx >= header.dinode_count:
                return inodes
            off = i * inode_size
            inode_blob = block[off : off + inode_size]
            inodes.append(parse_image_inode(inode_blob, inode_idx, signed=signed, inode_bits=inode_bits))
            inode_idx += 1
    return inodes

def inspect_pfs_image(image: Path, source: Path | None = None, expected_crc32: int | None = None, expected_manifest_sha256: str | None = None, ekpfs: bytes | None = None, new_crypt: bool = False) -> PFSImageInspection:
    inspection = PFSImageInspection(image=image, size_bytes=image.stat().st_size if image.exists() else 0)
    if not image.exists() or not image.is_file():
        inspection.errors.append(f"image path does not exist or is not a file: {image}")
        return inspection
    try:
        with image.open("rb") as fh:
            inspection.header = parse_image_header(fh)
            inspection.inodes = parse_image_inodes(fh, inspection.header, ekpfs, new_crypt)
            # ... (Continue with tree building and validation logic from original file) ...
    except Exception as exc:
        inspection.errors.append(f"failed to inspect image: {exc}")
    return inspection

def paths_have_fpt_collision(
    dirs_sorted: list[DirNode],
    files_sorted: list[FileNode],
    case_insensitive: bool = True,
) -> bool:
    """Return whether the source paths require a collision resolver.

    Args:
        dirs_sorted: Directory nodes considered for flat path table entries.
        files_sorted: File nodes considered for flat path table entries.
        case_insensitive: Whether hashes should use case-insensitive path folding.

    Returns:
        ``True`` when two or more paths produce the same FPT hash, otherwise
        ``False``.
    """
    seen_hashes: set[int] = set()
    path_value: str
    for directory_node in dirs_sorted:
        if directory_node.rel_dir == "":
            continue
        path_value = dir_full_path_for_hash(directory_node)
        path_hash: int = fpt_hash(path_value, case_insensitive=case_insensitive)
        if path_hash in seen_hashes:
            return True
        seen_hashes.add(path_hash)
    for file_node in files_sorted:
        path_value = file_full_path_for_hash(file_node)
        path_hash = fpt_hash(path_value, case_insensitive=case_insensitive)
        if path_hash in seen_hashes:
            return True
        seen_hashes.add(path_hash)
    return False

def file_full_path_for_hash(file_node: FileNode) -> str:
    return "/" + file_node.rel_path.replace("\\", "/")

def extract_pfs_image(image: Path, output_path: Path, progress=None, ekpfs: bytes | None = None, new_crypt: bool = False) -> PFSExtractionResult:
    result = PFSExtractionResult(image=image, output_path=output_path)
    inspection = inspect_pfs_image(image, ekpfs=ekpfs, new_crypt=new_crypt)
    result.errors.extend(inspection.errors)
    if result.errors or not inspection.header: return result
    
    output_path.mkdir(parents=True, exist_ok=True)
    try:
        with image.open("rb") as fh:
            for rel_path, inode_num in inspection.file_inodes.items():
                target = output_path / rel_path
                target.parent.mkdir(parents=True, exist_ok=True)
                inode = inspection.inodes[inode_num]
                with target.open("wb") as out_fh:
                    # Stream logical blocks
                    payload = read_image_inode_payload(fh, inspection.header, inode, ekpfs, new_crypt)
                    decoded = decode_inode_payload(payload, inode)
                    out_fh.write(decoded)
                    result.bytes_written += len(decoded)
                result.files_written += 1
    except Exception as exc:
        result.errors.append(f"failed to extract image: {exc}")
    return result

def validate_input(path: Path, require_game_files: bool = True) -> tuple[str | None, list[str]]:
    """Validate a source directory before packing.

    Args:
        path: Source directory to validate.
        require_game_files: When True, require the usual game-folder files,
            including ``sce_sys/param.json`` and ``eboot.bin``. When False,
            skip these checks and allow packing any directory tree.

    Returns:
        A tuple of ``(title_id, warnings)``. ``title_id`` is ``None`` when the
        relaxed mode skips game-file validation.

    Raises:
        BuildError: If the path is not a directory or strict validation fails.
    """
    if not path.exists() or not path.is_dir():
        raise BuildError(f"--path must be an existing directory: {path}")
    if not require_game_files:
        return None, []

    param_json = path / "sce_sys" / "param.json"
    if not param_json.exists():
        raise BuildError(f"Missing required file: {param_json}")

    parsed = read_param_json(param_json)
    title_id = parsed.get("titleId") or parsed.get("title_id")
    if not isinstance(title_id, str) or not title_id.strip():
        raise BuildError("param.json is missing a valid titleId/title_id")

    eboot_path = path / "eboot.bin"
    if not eboot_path.exists():
        raise BuildError(f"Missing required file: {eboot_path}")

    warnings: list[str] = []
    return title_id.strip(), warnings


def file_full_path_for_hash(file_node: FileNode) -> str:
    return "/" + file_node.rel_path.replace("\\", "/")


def dir_full_path_for_hash(dir_node: DirNode) -> str:
    if dir_node.rel_dir == "":
        return ""
    return "/" + dir_node.rel_dir.replace("\\", "/")


def fpt_hash(name: str, case_insensitive: bool = True) -> int:
    """Calculate flat_path_table hash.

    Args:
        name: Path to hash
        case_insensitive: If True, uppercase characters; if False, use as-is
    """
    h = 0
    for c in name:
        char = c.upper() if case_insensitive else c
        h = (ord(char) + (31 * h)) & 0xFFFFFFFF
    return h


def make_fpt_and_collision_blob(
    dirs_sorted: list[DirNode],
    files_sorted: list[FileNode],
    inode_by_path: dict[str, Inode],
    case_insensitive: bool = True,
) -> tuple[bytes, bytes | None, bool]:
    path_entries: list[tuple[str, int, bool]] = []
    for d in dirs_sorted:
        if d.rel_dir == "":
            continue
        path_entries.append((dir_full_path_for_hash(d), inode_by_path[f"dir:{d.rel_dir}"].number, True))
    for f in files_sorted:
        path_entries.append((file_full_path_for_hash(f), inode_by_path[f"file:{f.rel_path}"].number, False))

    by_hash: dict[int, list[tuple[str, int, bool]]] = {}
    for item in path_entries:
        h = fpt_hash(item[0], case_insensitive=case_insensitive)
        by_hash.setdefault(h, []).append(item)

    has_collision = any(len(v) > 1 for v in by_hash.values())

    hash_map: dict[int, int] = {}
    collision_blob = bytearray()
    collision_offsets: dict[int, int] = {}

    if has_collision:
        for h in sorted(by_hash.keys()):
            entries = by_hash[h]
            if len(entries) <= 1:
                continue
            offset = len(collision_blob)
            collision_offsets[h] = offset
            for full_path, ino_num, is_dir in entries:
                d = Dirent(
                    inode_number=ino_num,
                    type_code=consts.DIRENT_TYPE_DIRECTORY if is_dir else consts.DIRENT_TYPE_FILE,
                    name=full_path,
                )
                collision_blob += d.to_bytes()
            collision_blob += b"\x00" * 0x18

    for h in sorted(by_hash.keys()):
        entries = by_hash[h]
        if len(entries) == 1:
            _, ino_num, is_dir = entries[0]
            hash_map[h] = ino_num | (0x20000000 if is_dir else 0)
        else:
            hash_map[h] = 0x80000000 | collision_offsets[h]

    fpt = bytearray()
    for h in sorted(hash_map.keys()):
        fpt += struct.pack("<II", h, hash_map[h] & 0xFFFFFFFF)

    return bytes(fpt), (bytes(collision_blob) if has_collision else None), has_collision


def paths_have_fpt_collision(
    dirs_sorted: list[DirNode],
    files_sorted: list[FileNode],
    case_insensitive: bool = True,
) -> bool:
    """Return whether the source paths require a collision resolver.

    Args:
        dirs_sorted: Directory nodes considered for flat path table entries.
        files_sorted: File nodes considered for flat path table entries.
        case_insensitive: Whether hashes should use case-insensitive path folding.

    Returns:
        ``True`` when two or more paths produce the same FPT hash, otherwise
        ``False``.
    """
    seen_hashes: set[int] = set()
    path_value: str
    for directory_node in dirs_sorted:
        if directory_node.rel_dir == "":
            continue
        path_value = dir_full_path_for_hash(directory_node)
        path_hash: int = fpt_hash(path_value, case_insensitive=case_insensitive)
        if path_hash in seen_hashes:
            return True
        seen_hashes.add(path_hash)
    for file_node in files_sorted:
        path_value = file_full_path_for_hash(file_node)
        path_hash = fpt_hash(path_value, case_insensitive=case_insensitive)
        if path_hash in seen_hashes:
            return True
        seen_hashes.add(path_hash)
    return False


def compute_file_storage(
    file_node: FileNode,
    compress: bool,
    threshold_gain: int,
    min_file_gain: int = 0,
    min_compress_size: int = 0,
    block_size: int = consts.PFSC_LOGICAL_BLOCK_SIZE,
    zlib_level: int = 7,
) -> None:
    """Decide how a file will be stored in the image.

    This function updates the provided FileNode in-place with source-path based
    metadata and does not retain payload bytes in memory.

    Args:
        file_node: FileNode describing the file to process.
        compress: Whether compression is enabled.
        threshold_gain: Minimum percent gain required to keep compressed data.
        min_file_gain: Minimum whole-file gain percent required to store PFSC.
        min_compress_size: Minimum raw file size required before trying PFSC.
        block_size: PFSC logical block size used for compression planning.
        zlib_level: Compression level passed to zlib.compress.

    Raises:
        OSError: If reading the file from disk fails.
        ValueError: If threshold_gain is outside the 0..100 range.
    """
    # Validate compression parameters up front.
    if not (0 <= threshold_gain <= 100):
        raise ValueError(f"threshold_gain must be between 0 and 100 inclusive, got {threshold_gain}")

    raw_size: int = file_node.abs_path.stat().st_size
    if not compress or raw_size == 0 or raw_size < min_compress_size:
        file_node.stored_source_path = file_node.abs_path
        file_node.stored_source_is_temp = False
        file_node.stored_size = raw_size
        file_node.compressed = False
        file_node.gain_pct = 0.0
        file_node.hypothetical_compressed_size = 0
        return

    stored_size: int
    is_compressed: bool
    gain_pct: float
    hypothetical_size: int
    stored_size, is_compressed, gain_pct, hypothetical_size = _analyze_pfsc_file_storage(
        abs_path=file_node.abs_path,
        threshold_gain=threshold_gain,
        min_file_gain=min_file_gain,
        zlib_level=zlib_level,
        logical_block_size=block_size,
        block_worker_count=1,
    )
    file_node.stored_source_path = file_node.abs_path
    file_node.stored_source_is_temp = False
    file_node.stored_size = stored_size
    file_node.compressed = is_compressed
    file_node.gain_pct = gain_pct
    file_node.hypothetical_compressed_size = hypothetical_size


def _compute_file_storage_worker(
    args: tuple[Path, int, int, int, bool, int, int, bool, SupportsIntQueue | None, Path | None],
) -> tuple[Path, Path, bool, int, bool, float, int]:
    """Worker function for parallel compression.

    This function is executed in a worker process and performs the same storage
    decision logic as :func:`compute_file_storage` but returns the results instead
    of mutating a FileNode.

    Args:
        args: Tuple containing ``(abs_path, threshold_gain, min_file_gain, min_compress_size, compress, block_size,
            zlib_level, dry_run, progress_queue, temp_folder)``.

    Returns:
        A tuple ``(file_path, stored_source_path, stored_source_is_temp, stored_size,
        compressed, gain_pct, hypothetical_compressed_size)``.
    """
    abs_path: Path
    threshold_gain: int
    min_file_gain: int
    min_compress_size: int
    compress: bool
    _block_size: int
    zlib_level: int
    dry_run: bool
    progress_queue: SupportsIntQueue | None
    temp_folder: Path | None
    (
        abs_path,
        threshold_gain,
        min_file_gain,
        min_compress_size,
        compress,
        _block_size,
        zlib_level,
        dry_run,
        progress_queue,
        temp_folder,
    ) = args

    raw_size: int = abs_path.stat().st_size
    if not compress or raw_size == 0 or raw_size < min_compress_size:
        return abs_path, abs_path, False, raw_size, False, 0.0, 0

    batched_progress_bytes: int = 0

    def report_progress(delta_bytes: int) -> None:
        """Batch worker progress updates before pushing them to the parent."""
        nonlocal batched_progress_bytes
        batched_progress_bytes += delta_bytes
        if progress_queue is not None and batched_progress_bytes >= PFSC_PROGRESS_REPORT_BYTES:
            progress_queue.put(batched_progress_bytes)
            batched_progress_bytes = 0

    stored_size: int
    is_compressed: bool
    gain_pct: float
    hypothetical_compressed_size: int
    if dry_run:
        stored_size, is_compressed, gain_pct, hypothetical_compressed_size = _analyze_pfsc_file_storage(
            abs_path=abs_path,
            threshold_gain=threshold_gain,
            min_file_gain=min_file_gain,
            zlib_level=zlib_level,
            logical_block_size=consts.PFSC_LOGICAL_BLOCK_SIZE,
            block_worker_count=1,
            progress_callback=report_progress if progress_queue is not None else None,
        )
        stored_source_path: Path = abs_path
        stored_source_is_temp: bool = False
    else:
        spool_path: Path = _make_compression_spool_path(source_path=abs_path, temp_folder=temp_folder)
        stored_size, is_compressed, gain_pct, hypothetical_compressed_size = _encode_pfsc_file_to_spool(
            abs_path=abs_path,
            spool_path=spool_path,
            threshold_gain=threshold_gain,
            min_file_gain=min_file_gain,
            zlib_level=zlib_level,
            logical_block_size=consts.PFSC_LOGICAL_BLOCK_SIZE,
            block_worker_count=1,
            progress_callback=report_progress if progress_queue is not None else None,
        )
        if is_compressed:
            stored_source_path = spool_path
            stored_source_is_temp = True
        else:
            with suppress(FileNotFoundError):
                spool_path.unlink()
            stored_source_path = abs_path
            stored_source_is_temp = False
    if progress_queue is not None and batched_progress_bytes > 0:
        progress_queue.put(batched_progress_bytes)
    return (
        abs_path,
        stored_source_path,
        stored_source_is_temp,
        stored_size,
        is_compressed,
        gain_pct,
        hypothetical_compressed_size,
    )


def scan_source_tree(root: Path, progress: Progress) -> tuple[dict[str, DirNode], dict[str, FileNode], int]:
    """Scan a source directory tree and return DirNode/FileNode maps.

    The returned structures mirror what the older monolithic implementation
    produced. This helper is used by the build flow and must preserve
    determinism and ordering.

    Args:
        root: Path to the directory to scan.
        progress: Progress instance used to report scanning progress.

    Returns:
        A tuple of (dirs, files, total_files) where dirs and files are maps keyed
        by relative path and total_files is the number of files discovered.
    """
    progress.status("\nDiscovering files...")
    abs_files: list[Path] = [p for p in root.rglob("*") if p.is_file()]
    abs_files.sort(key=lambda p: p.relative_to(root).as_posix().lower())

    # Validate filenames before compression work begins; non-ASCII names are unsupported.
    non_ascii_paths: list[str] = []
    for abs_path in abs_files:
        rel_path: Path = abs_path.relative_to(root)
        rel_str: str = rel_path.as_posix()
        for part in rel_path.parts:
            if not part.isascii():
                non_ascii_paths.append(rel_str)
                break
    if non_ascii_paths:
        offenders: str = "\n  ".join(non_ascii_paths)
        raise BuildError(
            f"Source tree contains {len(non_ascii_paths)} file(s) with non-ASCII names."
            f" PFS images only support ASCII filenames:\n  {offenders}"
        )

    dirs: dict[str, DirNode] = {"": DirNode(rel_dir="", name="uroot", parent_rel_dir=None)}
    files: dict[str, FileNode] = {}

    total: int = len(abs_files)
    total_bytes: int = 0
    for i, abs_path in enumerate(abs_files, start=1):
        rel: str = abs_path.relative_to(root).as_posix()
        parent: str = str(Path(rel).parent.as_posix())
        if parent == ".":
            parent = ""
        parts: list[str] = list(Path(rel).parts[:-1])

        curr: str = ""
        for part in parts:  # pragma: no cover - exercised indirectly in integration tests
            next_rel: str = f"{curr}/{part}" if curr else part
            if next_rel not in dirs:
                dirs[next_rel] = DirNode(rel_dir=next_rel, name=part, parent_rel_dir=curr if curr != "" else "")
                dirs[curr].children_dirs.append(next_rel)
            curr = next_rel

        if parent not in dirs:  # pragma: no cover - defensive fallback
            # This should not happen but keep it robust.
            dirs[parent] = DirNode(
                rel_dir=parent, name=Path(parent).name if parent else "uroot", parent_rel_dir=""
            )  # pragma: no cover

        name: str = Path(rel).name  # pragma: no cover - defensive path
        raw_size: int = abs_path.stat().st_size
        total_bytes += raw_size
        file_node: FileNode = FileNode(
            rel_path=rel,
            abs_path=abs_path,
            parent_rel_dir=parent,
            name=name,
            raw_size=raw_size,
        )
        files[rel] = file_node
        dirs[parent].children_files.append(rel)
        progress.step("scan", i, total, bytes_processed=total_bytes)

    for d in dirs.values():
        d.children_dirs.sort(key=str.lower)
        d.children_files.sort(key=str.lower)

    return dirs, files, total

def parse_sig_record_block(
    fh: BinaryIO,
    block_num: int,
    inode_bits: int,
    header: ParsedHeader | None = None,
    block_size: int | None = None,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> list[tuple[bytes, int]]:
    """Parse one indirect signature-record block from an image.

    Args:
        fh: Open image file handle.
        block_num: Filesystem block number containing the record list.
        inode_bits: Signed inode width, 32 or 64.
        header: Parsed image header, or ``None`` when reading an already-decrypted
            raw block blob.
        block_size: Optional explicit block size for compatibility with older callers.
        ekpfs: Optional EKPFS key material. Defaults to the all-zero key.
        new_crypt: When True, use the alternate newCrypt key derivation path.

    Returns:
        Parsed `(signature, block_number)` tuples.
    """
    if header is None:
        if block_size is None:
            raise ValueError("block_size is required when header is not provided")
        resolved_block_size: int = block_size
        blob: bytes = _read_exact(fh, block_num * resolved_block_size, resolved_block_size)
    else:
        resolved_block_size = header.block_size if block_size is None else block_size
        blob = read_image_bytes(
            fh,
            header,
            block_num * resolved_block_size,
            resolved_block_size,
            ekpfs=ekpfs,
            new_crypt=new_crypt,
        )
    layout: SignedInodeLayout = signed_inode_layout(inode_bits)
    records: list[tuple[bytes, int]] = []
    for offset in range(0, resolved_block_size, layout.entry_size):
        if offset + layout.entry_size > resolved_block_size:
            break
        sig = blob[offset : offset + consts.SIG_SIZE]
        block = struct.unpack_from(layout.block_format, blob, offset + consts.SIG_SIZE)[0]
        records.append((sig, block))
    return records

def verify_signed_image_signatures(
    fh: BinaryIO,
    header: ParsedHeader,
    inodes: list[ParsedInode],
    errors: list[str],
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> None:
    if (header.mode & consts.PFS_MODE_SIGNED) == 0:
        return

    sign_key = pfs_gen_sign_key(resolve_ekpfs_key(ekpfs=ekpfs), header.seed)
    inode_bits: int = signed_inode_bits_from_mode(header.mode)
    layout: SignedInodeLayout = signed_inode_layout(inode_bits)

    for i in range(header.dinode_block_count):
        block_num = 1 + i
        block_data = read_image_bytes(
            fh, header, block_num * header.block_size, header.block_size, ekpfs=ekpfs, new_crypt=new_crypt
        )
        sig_offset = header_inode_block_sig_offset(i)
        expected = hmac_sha256(sign_key, block_hmac_without_slot(block_data, 0, header.block_size, signed=False))
        actual = _read_exact(fh, sig_offset, consts.SIG_SIZE)
        if actual != expected:
            errors.append(f"inode block signature mismatch for block {block_num}")

    header_region = bytearray(_read_exact(fh, 0, consts.HEADER_DIGEST_SIZE))
    header_region[consts.HEADER_DIGEST_OFFSET : consts.HEADER_DIGEST_OFFSET + consts.SIG_SIZE] = (
        b"\x00" * consts.SIG_SIZE
    )
    expected_header_sig = hmac_sha256(sign_key, bytes(header_region))
    actual_header_sig = _read_exact(fh, consts.HEADER_DIGEST_OFFSET, consts.SIG_SIZE)
    if actual_header_sig != expected_header_sig:
        errors.append("header signature region digest mismatch")

    for inode in inodes:
        remaining = inode.blocks
        direct_count = min(remaining, consts.MAX_DIRECT_BLOCKS)
        for idx in range(direct_count):
            block = inode.db[idx]
            if block <= 0:
                errors.append(f"inode {inode.number} has invalid direct block db[{idx}]={block}")
                continue
            block_data = read_image_bytes(
                fh, header, block * header.block_size, header.block_size, ekpfs=ekpfs, new_crypt=new_crypt
            )
            expected = hmac_sha256(sign_key, block_data)
            actual = inode.db_sig[idx]
            if actual != expected:
                errors.append(f"inode {inode.number} direct signature mismatch at db[{idx}] -> block {block}")
        remaining -= direct_count

        sigs_per_block = header.block_size // layout.entry_size
        if remaining > 0:
            ib0 = inode.ib[0]
            if ib0 <= 0:
                errors.append(f"inode {inode.number} missing ib[0] for signed block chain")
            else:
                ib0_data = read_image_bytes(
                    fh, header, ib0 * header.block_size, header.block_size, ekpfs=ekpfs, new_crypt=new_crypt
                )
                if inode.ib_sig[0] != hmac_sha256(sign_key, ib0_data):
                    errors.append(f"inode {inode.number} indirect signature mismatch at ib[0] -> block {ib0}")
                    records = parse_sig_record_block(
                        fh, ib0, inode_bits, header=header, ekpfs=ekpfs, new_crypt=new_crypt
                    )
                take = min(remaining, sigs_per_block)
                for rec_idx, (sig, block) in enumerate(records[:take]):
                    if block <= 0:
                        errors.append(f"inode {inode.number} ib[0] record {rec_idx} has invalid block {block}")
                        continue
                    expected = hmac_sha256(
                        sign_key,
                        read_image_bytes(
                            fh,
                            header,
                            block * header.block_size,
                            header.block_size,
                            ekpfs=ekpfs,
                            new_crypt=new_crypt,
                        ),
                    )
                    if sig != expected:
                        errors.append(
                            f"inode {inode.number} ib[0] record {rec_idx} signature mismatch for block {block}"
                        )
                remaining -= take

        if remaining > 0:
            ib1 = inode.ib[1]
            if ib1 <= 0:
                errors.append(f"inode {inode.number} missing ib[1] for signed block chain")
            else:
                ib1_data = read_image_bytes(
                    fh, header, ib1 * header.block_size, header.block_size, ekpfs=ekpfs, new_crypt=new_crypt
                )
                if inode.ib_sig[1] != hmac_sha256(sign_key, ib1_data):
                    errors.append(f"inode {inode.number} indirect signature mismatch at ib[1] -> block {ib1}")
                parent_records = parse_sig_record_block(
                    fh, ib1, inode_bits, header=header, ekpfs=ekpfs, new_crypt=new_crypt
                )
                for parent_idx, (parent_sig, child_indirect) in enumerate(parent_records):
                    if remaining <= 0:
                        break
                    if child_indirect <= 0:
                        errors.append(
                            f"inode {inode.number} ib[1] record {parent_idx} has invalid block {child_indirect}"
                        )
                        continue
                    child_data = read_image_bytes(
                        fh,
                        header,
                        child_indirect * header.block_size,
                        header.block_size,
                        ekpfs=ekpfs,
                        new_crypt=new_crypt,
                    )
                    if parent_sig != hmac_sha256(sign_key, child_data):
                        errors.append(
                            f"inode {inode.number} ib[1] record {parent_idx} "
                            f"signature mismatch for block {child_indirect}"
                        )
                    child_records = parse_sig_record_block(
                        fh, child_indirect, inode_bits, header=header, ekpfs=ekpfs, new_crypt=new_crypt
                    )
                    take = min(remaining, sigs_per_block)
                    for rec_idx, (sig, block) in enumerate(child_records[:take]):
                        if block <= 0:
                            errors.append(
                                f"inode {inode.number} ib[1][{parent_idx}] record {rec_idx} has invalid block {block}"
                            )
                            continue
                        expected = hmac_sha256(
                            sign_key,
                            read_image_bytes(
                                fh,
                                header,
                                block * header.block_size,
                                header.block_size,
                                ekpfs=ekpfs,
                                new_crypt=new_crypt,
                            ),
                        )
                        if sig != expected:
                            errors.append(
                                f"inode {inode.number} ib[1][{parent_idx}] record {rec_idx} "
                                f"signature mismatch for block {block}"
                            )
                    remaining -= take

        if remaining > 0:
            errors.append(f"inode {inode.number} exceeds supported signed verification depth")


def resolve_signed_inode_blocks(
    fh: BinaryIO,
    header: ParsedHeader,
    inode: ParsedInode,
    errors: list[str] | None = None,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> list[int]:
    blocks: list[int] = []
    direct_count = min(inode.blocks, consts.MAX_DIRECT_BLOCKS)
    blocks.extend(inode.db[:direct_count])
    remaining = inode.blocks - direct_count
    inode_bits: int = signed_inode_bits_from_mode(header.mode)
    layout: SignedInodeLayout = signed_inode_layout(inode_bits)
    sigs_per_block: int = header.block_size // layout.entry_size

    if remaining > 0:
        if inode.ib[0] <= 0:
            if errors is not None:
                errors.append(f"inode {inode.number} missing ib[0] for signed block chain")
            return blocks
        records = parse_sig_record_block(fh, inode.ib[0], inode_bits, header=header, ekpfs=ekpfs, new_crypt=new_crypt)
        take = min(remaining, sigs_per_block)
        blocks.extend(block for _sig, block in records[:take])
        remaining -= take

    if remaining > 0:
        if inode.ib[1] <= 0:
            if errors is not None:
                errors.append(f"inode {inode.number} missing ib[1] for signed block chain")
            return blocks
        parent_records = parse_sig_record_block(
            fh, inode.ib[1], inode_bits, header=header, ekpfs=ekpfs, new_crypt=new_crypt
        )
        for _sig, child_block in parent_records:
            if remaining <= 0:
                break
            child_records = parse_sig_record_block(
                fh, child_block, inode_bits, header=header, ekpfs=ekpfs, new_crypt=new_crypt
            )
            take = min(remaining, sigs_per_block)
            blocks.extend(block for _sig2, block in child_records[:take])
            remaining -= take

    if remaining > 0 and errors is not None:
        errors.append(f"inode {inode.number} uses unsupported signed indirection depth")
    return blocks


def parse_image_dirents(blob: bytes, strict: bool = False) -> tuple[list[ParsedDirent], list[str]]:
    dirents: list[ParsedDirent] = []
    errors: list[str] = []
    offset = 0
    while offset + 16 <= len(blob):
        inode_number, type_code, name_len, ent_size = struct.unpack_from("<Iiii", blob, offset)
        if inode_number == 0 and type_code == 0 and name_len == 0 and ent_size == 0:
            break

        if ent_size < 17 or (ent_size % 8) != 0:
            msg = f"invalid dirent size {ent_size} at offset {offset}"
            if strict:
                errors.append(msg)
            break
        if name_len < 0 or name_len > ent_size - 16:
            msg = f"invalid dirent name length {name_len} at offset {offset}"
            if strict:
                errors.append(msg)
            break
        if offset + ent_size > len(blob):
            msg = f"dirent at offset {offset} exceeds payload boundary"
            if strict:
                errors.append(msg)
            break

        name_bytes = blob[offset + 16 : offset + 16 + name_len]
        try:
            name = name_bytes.decode("ascii", errors="strict")
        except UnicodeDecodeError:
            name = name_bytes.decode("ascii", errors="replace")
            if strict:
                errors.append(f"non-ascii dirent name at offset {offset}")

        dirents.append(ParsedDirent(inode_number=inode_number, type_code=type_code, name=name))
        offset += ent_size

    return dirents, errors


def read_image_inode_payload(
    fh: BinaryIO,
    header: ParsedHeader,
    inode: ParsedInode,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> bytes:
    """Read one inode payload, decrypting encrypted images transparently.

    Args:
        fh: Open image file handle.
        header: Parsed image header.
        inode: Parsed inode whose payload should be read.
        ekpfs: Optional EKPFS key material. Defaults to the all-zero key.
        new_crypt: When True, use the alternate newCrypt key derivation path.

    Returns:
        Stored payload bytes for the inode.

    Raises:
        ValueError: If inode sizes are invalid or payload bytes are truncated.
    """
    if inode.blocks <= 0:
        return b""
    payload_size: int = inode.stored_size
    if payload_size < 0:
        raise ValueError(f"inode {inode.number} has negative stored payload size")
    if inode.db_sig or inode.ib_sig:
        block_numbers = resolve_signed_inode_blocks(
            fh,
            header,
            inode,
            ekpfs=ekpfs,
            new_crypt=new_crypt,
        )
        data = bytearray()
        for block in block_numbers:
            data += read_image_bytes(
                fh,
                header,
                block * header.block_size,
                header.block_size,
                ekpfs=ekpfs,
                new_crypt=new_crypt,
            )
        data = bytes(data[:payload_size])
    else:
        data = read_image_bytes(
            fh,
            header,
            inode.db[0] * header.block_size,
            payload_size,
            ekpfs=ekpfs,
            new_crypt=new_crypt,
        )
    if len(data) != payload_size:
        raise ValueError(f"inode {inode.number} payload truncated")
    return data


def iter_inode_logical_blocks(
    fh: BinaryIO,
    header: ParsedHeader,
    inode: ParsedInode,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
    chunk_size: int = 4 * 1024 * 1024,
) -> Iterator[bytes]:
    """Yield an inode's logical payload in bounded-memory chunks.

    Streams the decoded payload without ever holding the whole file in memory:
    for unsigned PFSC payloads it reads and decompresses one logical block at a
    time; for unsigned raw payloads it copies fixed-size chunks; signed payloads
    (size-limited by the signed layout) fall back to a single buffered decode.

    Args:
        fh: Open image file handle.
        header: Parsed image header.
        inode: Parsed inode whose payload should be streamed.
        ekpfs: Optional EKPFS key material. Defaults to the all-zero key.
        new_crypt: When True, use the alternate newCrypt key derivation path.
        chunk_size: Read chunk size for raw (uncompressed) payloads.

    Yields:
        Logical payload byte chunks in order; concatenation equals the decoded file.

    Raises:
        ValueError: If the stored payload structure is invalid.
    """
    if inode.blocks <= 0 or inode.logical_size <= 0:
        return

    # Signed payloads are bounded by the signed-layout size limit; decode buffered.
    if inode.db_sig or inode.ib_sig:
        payload: bytes = read_image_inode_payload(fh, header, inode, ekpfs=ekpfs, new_crypt=new_crypt)
        yield decode_inode_payload(payload=payload, inode=inode)
        return

    base: int = inode.db[0] * header.block_size
    expected: int = inode.logical_size

    # Raw (uncompressed) contiguous payload: stored bytes equal logical bytes.
    if not inode.is_compressed:
        remaining: int = expected
        offset: int = base
        while remaining > 0:
            take: int = min(chunk_size, remaining)
            yield read_image_bytes(fh, header, offset, take, ekpfs=ekpfs, new_crypt=new_crypt)
            offset += take
            remaining -= take
        return

    # Compressed PFSC payload stored contiguously from ``base``.
    stored_size: int = inode.stored_size
    head: bytes = read_image_bytes(fh, header, base, consts.PFSC_HEADER_SIZE, ekpfs=ekpfs, new_crypt=new_crypt)
    logical_block_size, block_count, block_offsets_offset, data_offset, pfsc_logical_size = _parse_pfsc_header(head)
    if data_offset > stored_size:
        raise ValueError("PFSC data offset exceeds stored payload length")
    offsets_size: int = (block_count + 1) * consts.PFSC_OFFSET_ENTRY_SIZE
    if block_offsets_offset + offsets_size > data_offset or block_offsets_offset + offsets_size > stored_size:
        raise ValueError("PFSC payload is truncated before block offset table")

    offset_table: bytes = read_image_bytes(
        fh, header, base + block_offsets_offset, offsets_size, ekpfs=ekpfs, new_crypt=new_crypt
    )
    offsets: list[int] = list(struct.unpack_from(f"<{block_count + 1}Q", offset_table, 0))
    if offsets[0] != data_offset:
        raise ValueError("PFSC block offsets must start at data_start")
    if offsets[-1] > stored_size:
        raise ValueError("PFSC block offsets exceed payload size")
    for idx in range(1, len(offsets)):
        if offsets[idx] < offsets[idx - 1]:
            raise ValueError("PFSC block offsets are not monotonic")
    if expected > pfsc_logical_size:
        raise ValueError(f"PFSC logical size {pfsc_logical_size} is smaller than inode size {expected}")

    # Read, decompress, and emit one logical block at a time, trimming to the file size.
    emitted: int = 0
    for idx in range(block_count):
        if emitted >= expected:
            break
        stored_block: bytes = read_image_bytes(
            fh, header, base + offsets[idx], offsets[idx + 1] - offsets[idx], ekpfs=ekpfs, new_crypt=new_crypt
        )
        logical_block: bytes = _decode_pfsc_block(stored_block, logical_block_size, idx)
        if emitted + len(logical_block) > expected:
            logical_block = logical_block[: expected - emitted]
        emitted += len(logical_block)
        yield logical_block

    if emitted != expected:
        raise ValueError(f"PFSC streamed output size {emitted} does not match inode size {expected}")


def parse_superroot_and_indexes(
    fh: BinaryIO,
    header: ParsedHeader,
    inodes: list[ParsedInode],
    errors: list[str],
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> tuple[int, dict[int, int], dict[int, list[ParsedDirent]], set[int]]:
    super_root_offset = (1 + header.dinode_block_count) * header.block_size
    blob: bytes = read_image_bytes(fh, header, super_root_offset, header.block_size, ekpfs=ekpfs, new_crypt=new_crypt)
    super_entries, parse_errors = parse_image_dirents(blob, strict=True)
    for e in parse_errors:
        errors.append(f"superroot: {e}")

    fpt_inode = None
    collision_inode = None
    uroot_inode = None
    special_inodes: set[int] = {0}
    for ent in super_entries:
        if ent.name == "flat_path_table":
            fpt_inode = ent.inode_number
        elif ent.name == "collision_resolver":
            collision_inode = ent.inode_number
        elif ent.name == "uroot":
            uroot_inode = ent.inode_number

    if fpt_inode is None:
        errors.append("superroot missing 'flat_path_table' entry")
    if uroot_inode is None:
        errors.append("superroot missing 'uroot' entry")

    if fpt_inode is not None:
        special_inodes.add(fpt_inode)
    if collision_inode is not None:
        special_inodes.add(collision_inode)
    if uroot_inode is not None:
        special_inodes.add(uroot_inode)

    fpt_map: dict[int, int] = {}
    collision_map: dict[int, list[ParsedDirent]] = {}

    if fpt_inode is not None and 0 <= fpt_inode < len(inodes):
        fpt_blob = read_image_inode_payload(fh, header, inodes[fpt_inode], ekpfs=ekpfs, new_crypt=new_crypt)
        if (len(fpt_blob) % 8) != 0:
            errors.append("flat_path_table size is not divisible by 8")

        for i in range(0, len(fpt_blob) - (len(fpt_blob) % 8), 8):
            h, v = struct.unpack_from("<II", fpt_blob, i)
            if h in fpt_map:
                errors.append(f"flat_path_table has duplicate hash 0x{h:08X}")
            fpt_map[h] = v

        if any((v & 0x80000000) for v in fpt_map.values()):
            if collision_inode is None:
                errors.append("flat_path_table has collision entries but no collision_resolver inode")
            elif 0 <= collision_inode < len(inodes):
                c_blob = read_image_inode_payload(
                    fh, header, inodes[collision_inode], ekpfs=ekpfs, new_crypt=new_crypt
                )
                for h, v in fpt_map.items():
                    if (v & 0x80000000) == 0:
                        continue
                    offset = v & 0x7FFFFFFF
                    if offset >= len(c_blob):
                        errors.append(f"collision_resolver offset {offset} out of range for hash 0x{h:08X}")
                        continue
                    entries, parse_err = parse_image_dirents(c_blob[offset:], strict=True)
                    if parse_err:
                        errors.extend([f"collision_resolver hash 0x{h:08X}: {e}" for e in parse_err])
                    collision_map[h] = entries

    return (uroot_inode if uroot_inode is not None else -1), fpt_map, collision_map, special_inodes


def build_tree_from_uroot(
    fh: BinaryIO,
    header: ParsedHeader,
    inodes: list[ParsedInode],
    uroot_inode: int,
    errors: list[str],
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> tuple[dict[str, int], dict[str, int], dict[int, list[ParsedDirent]]]:
    files: dict[str, int] = {}
    dirs: dict[str, int] = {"": uroot_inode}
    dirents_by_inode: dict[int, list[ParsedDirent]] = {}
    visited: set[int] = set()
    dir_path_by_inode: dict[int, str] = {uroot_inode: ""}

    def walk(dir_inode_num: int, rel_path: str, parent_inode_num: int, ancestors: set[int]) -> None:
        if dir_inode_num in visited:
            return
        visited.add(dir_inode_num)

        if not (0 <= dir_inode_num < len(inodes)):
            errors.append(f"directory inode {dir_inode_num} is out of range")
            return

        inode = inodes[dir_inode_num]
        if not inode.is_dir:
            errors.append(f"inode {dir_inode_num} referenced as directory but mode is 0x{inode.mode:04X}")
            return

        payload = read_image_inode_payload(fh, header, inode, ekpfs=ekpfs, new_crypt=new_crypt)
        entries, parse_errors = parse_image_dirents(payload, strict=True)
        dirents_by_inode[dir_inode_num] = entries
        for e in parse_errors:
            errors.append(f"inode {dir_inode_num}: {e}")

        dot_entries = [e for e in entries if e.name == "."]
        dotdot_entries = [e for e in entries if e.name == ".."]
        dot = dot_entries[0] if dot_entries else None
        dotdot = dotdot_entries[0] if dotdot_entries else None

        if len(dot_entries) != 1:
            errors.append(f"directory '{rel_path or '/'}' must contain exactly one '.' entry")
        if dot is None:
            errors.append(f"directory '{rel_path or '/'}' missing '.' entry")
        elif dot.inode_number != dir_inode_num:
            errors.append(f"directory '{rel_path or '/'}' has '.' -> {dot.inode_number}, expected {dir_inode_num}")
        elif dot.type_code != consts.DIRENT_TYPE_DOT:
            errors.append(f"directory '{rel_path or '/'}' has '.' with invalid type {dot.type_code}")

        if len(dotdot_entries) != 1:
            errors.append(f"directory '{rel_path or '/'}' must contain exactly one '..' entry")
        if dotdot is None:
            errors.append(f"directory '{rel_path or '/'}' missing '..' entry")
        else:
            expected_parent = dir_inode_num if rel_path == "" else parent_inode_num
            if dotdot.inode_number != expected_parent:
                errors.append(
                    f"directory '{rel_path or '/'}' has '..' -> {dotdot.inode_number}, expected {expected_parent}"
                )
            if dotdot.type_code != consts.DIRENT_TYPE_DOTDOT:
                errors.append(f"directory '{rel_path or '/'}' has '..' with invalid type {dotdot.type_code}")

        names_seen: set[str] = set()
        next_ancestors = set(ancestors)
        next_ancestors.add(dir_inode_num)
        for ent in entries:
            if ent.name in (".", ".."):
                continue
            if ent.name in names_seen:
                errors.append(f"directory '{rel_path or '/'}' has duplicate entry '{ent.name}'")
                continue
            names_seen.add(ent.name)
            if "/" in ent.name:
                errors.append(f"directory '{rel_path or '/'}' has invalid entry name containing '/': {ent.name}")
                continue

            child_path = ent.name if rel_path == "" else f"{rel_path}/{ent.name}"
            if not (0 <= ent.inode_number < len(inodes)):
                errors.append(f"entry '{child_path}' references out-of-range inode {ent.inode_number}")
                continue

            child_inode = inodes[ent.inode_number]
            if ent.type_code == consts.DIRENT_TYPE_DIRECTORY:
                if not child_inode.is_dir:
                    errors.append(f"entry '{child_path}' typed directory but inode mode is 0x{child_inode.mode:04X}")
                    continue
                if ent.inode_number in next_ancestors:
                    errors.append(f"directory cycle detected at '{child_path}' (inode {ent.inode_number})")
                    continue
                prev_path = dir_path_by_inode.get(ent.inode_number)
                if prev_path is not None and prev_path != child_path:
                    errors.append(
                        f"directory inode {ent.inode_number} is reachable from multiple paths: "
                        f"'{prev_path}' and '{child_path}'"
                    )
                    continue
                dir_path_by_inode[ent.inode_number] = child_path
                dirs[child_path] = ent.inode_number
                walk(ent.inode_number, child_path, dir_inode_num, next_ancestors)
            elif ent.type_code == consts.DIRENT_TYPE_FILE:
                if not child_inode.is_file:
                    errors.append(f"entry '{child_path}' typed file but inode mode is 0x{child_inode.mode:04X}")
                    continue
                files[child_path] = ent.inode_number
            else:
                errors.append(f"directory '{rel_path or '/'}' has unsupported dirent type {ent.type_code}")

    walk(uroot_inode, "", uroot_inode, set())
    return files, dirs, dirents_by_inode


def verify_file_payload_hashes(
    fh: BinaryIO,
    header: ParsedHeader,
    inodes: list[ParsedInode],
    file_inodes: dict[str, int],
    errors: list[str],
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
    progress: Progress | None = None,
) -> tuple[int, int, str]:
    manifest = hashlib.sha256()
    cumulative_crc = 0
    checked = 0

    # Report progress against the total logical bytes to hash, throttled by volume.
    total_bytes: int = sum(max(0, inodes[n].logical_size) for n in file_inodes.values())
    progress_total: int = max(total_bytes, 1)
    processed: int = 0
    last_reported: int = 0
    update_interval: int = 8 * 1024 * 1024
    if progress is not None:
        progress.step("verify", 0, progress_total, bytes_processed=0)

    for rel in sorted(file_inodes.keys()):
        inode_num = file_inodes[rel]
        inode = inodes[inode_num]
        # Stream the logical payload one block at a time to keep memory flat.
        file_hash = hashlib.sha256()
        file_len = 0
        try:
            for chunk in iter_inode_logical_blocks(fh, header, inode, ekpfs=ekpfs, new_crypt=new_crypt):
                file_hash.update(chunk)
                cumulative_crc = zlib.crc32(chunk, cumulative_crc) & 0xFFFFFFFF
                file_len += len(chunk)
                processed += len(chunk)
                if progress is not None and processed - last_reported >= update_interval:
                    last_reported = processed
                    progress.step("verify", min(processed, total_bytes), progress_total, bytes_processed=processed)
        except (ValueError, OSError) as exc:
            errors.append(f"failed to read file payload '{rel}' (inode {inode_num}): {exc}")
            continue

        if inode.logical_size >= 0 and file_len != inode.logical_size:
            errors.append(f"file '{rel}' size {file_len} does not match inode size {inode.logical_size}")

        manifest.update(rel.encode("utf-8", errors="replace"))
        manifest.update(b"\0")
        manifest.update(file_hash.digest())
        checked += 1

    if progress is not None:
        progress.step("verify", progress_total, progress_total, bytes_processed=total_bytes)

    return checked, cumulative_crc, manifest.hexdigest()


def render_tree(dirents_by_inode: dict[int, list[ParsedDirent]], inode_num: int, prefix: str = "") -> list[str]:
    lines: list[str] = []
    entries = [e for e in dirents_by_inode.get(inode_num, []) if e.name not in (".", "..")]
    entries.sort(key=lambda e: (e.type_code != consts.DIRENT_TYPE_DIRECTORY, e.name.lower(), e.name))

    for idx, ent in enumerate(entries):
        last = idx == (len(entries) - 1)
        branch = "`-- " if last else "|-- "
        lines.append(prefix + branch + ent.name)
        if ent.type_code == consts.DIRENT_TYPE_DIRECTORY:
            child_prefix = prefix + ("    " if last else "|   ")
            lines.extend(render_tree(dirents_by_inode, ent.inode_number, child_prefix))
    return lines


def validate_inode_layout(
    header: ParsedHeader, inodes: list[ParsedInode], errors: list[str], warnings: list[str]
) -> None:
    if header.magic != consts.PFS_MAGIC:
        errors.append(f"header magic mismatch: 0x{header.magic:016X} != 0x{consts.PFS_MAGIC:016X}")
    if header.block_size <= 0 or (header.block_size & (header.block_size - 1)) != 0:
        errors.append(f"invalid block size {header.block_size}")
    if header.readonly != 1:
        warnings.append(f"header readonly byte is {header.readonly}, expected 1")
    if header.dinode_count != len(inodes):
        errors.append(f"inode count mismatch: header={header.dinode_count} parsed={len(inodes)}")

    used_ranges: list[tuple[int, int, int]] = []
    for inode in inodes:
        if inode.blocks <= 0:
            continue
        start = inode.db[0]
        end = start + inode.blocks - 1
        if start < 0:
            errors.append(f"inode {inode.number} has negative db[0]={start}")
            continue
        if end >= header.ndblock:
            errors.append(f"inode {inode.number} range [{start},{end}] exceeds ndblock {header.ndblock}")
        used_ranges.append((start, end, inode.number))

    used_ranges.sort()
    for i in range(1, len(used_ranges)):
        prev_start, prev_end, prev_ino = used_ranges[i - 1]
        curr_start, curr_end, curr_ino = used_ranges[i]
        if curr_start <= prev_end:
            errors.append(
                f"block overlap between inode {prev_ino} "
                f"[{prev_start},{prev_end}] and inode {curr_ino} [{curr_start},{curr_end}]"
            )


def build_expected_fpt(
    file_inodes: dict[str, int], dir_inodes: dict[str, int], case_insensitive: bool
) -> dict[int, list[tuple[str, bool, int]]]:
    out: dict[int, list[tuple[str, bool, int]]] = {}
    for rel_dir, inode_num in dir_inodes.items():
        if rel_dir == "":
            continue
        full = "/" + rel_dir
        h = fpt_hash(full, case_insensitive=case_insensitive)
        out.setdefault(h, []).append((full, True, inode_num))
    for rel_file, inode_num in file_inodes.items():
        full = "/" + rel_file
        h = fpt_hash(full, case_insensitive=case_insensitive)
        out.setdefault(h, []).append((full, False, inode_num))
    return out


def validate_fpt_maps(
    fpt_map: dict[int, int],
    collision_map: dict[int, list[ParsedDirent]],
    expected: dict[int, list[tuple[str, bool, int]]],
    errors: list[str],
) -> None:
    expected_hashes = set(expected.keys())
    table_hashes = set(fpt_map.keys())

    for h in sorted(expected_hashes - table_hashes):
        errors.append(f"flat_path_table missing hash 0x{h:08X}")
    for h in sorted(table_hashes - expected_hashes):
        errors.append(f"flat_path_table has unexpected hash 0x{h:08X}")

    for h in sorted(expected_hashes & table_hashes):
        exp_entries = expected[h]
        val = fpt_map[h]
        if len(exp_entries) == 1:
            exp_path, exp_is_dir, exp_inode = exp_entries[0]
            if val & 0x80000000:
                errors.append(f"hash 0x{h:08X} for {exp_path} unexpectedly points to collision resolver")
                continue
            act_is_dir = bool(val & 0x20000000)
            act_inode = val & 0x1FFFFFFF
            if act_is_dir != exp_is_dir or act_inode != exp_inode:
                errors.append(
                    f"hash 0x{h:08X} mismatch: actual inode={act_inode} dir={act_is_dir}, "
                    f"expected inode={exp_inode} dir={exp_is_dir} ({exp_path})"
                )
        else:
            if (val & 0x80000000) == 0:
                errors.append(f"hash 0x{h:08X} has collisions but does not point to collision resolver")
                continue
            actual_set = {
                (e.name, e.type_code == consts.DIRENT_TYPE_DIRECTORY, e.inode_number) for e in collision_map.get(h, [])
            }
            expected_set = set(exp_entries)
            if not expected_set.issubset(actual_set):
                errors.append(f"collision resolver for hash 0x{h:08X} is missing expected entries")


def validate_ps5_checklist(
    fh: BinaryIO,
    header: ParsedHeader,
    inodes: list[ParsedInode],
    file_inodes: dict[str, int],
    warnings: list[str],
    errors: list[str],
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> None:
    if "sce_sys/param.json" in file_inodes:
        inode = inodes[file_inodes["sce_sys/param.json"]]
        payload = read_image_inode_payload(fh, header, inode, ekpfs=ekpfs, new_crypt=new_crypt)
        if inode.is_compressed:
            try:
                payload = decode_inode_payload(payload=payload, inode=inode)
            except ValueError as exc:
                errors.append(f"sce_sys/param.json payload decode failed: {exc}")
                payload = b""
        if payload:
            try:
                parsed = json.loads(payload.decode("utf-8"))
                if not parsed.get("titleId") and not parsed.get("title_id"):
                    warnings.append("sce_sys/param.json missing titleId/title_id")
            except Exception as exc:
                errors.append(f"sce_sys/param.json invalid JSON: {exc}")
    else:
        warnings.append("sce_sys/param.json not found")

    if "eboot.bin" not in file_inodes:
        warnings.append("eboot.bin not found")
    if "sce_sys/pfs-version.dat" not in file_inodes:
        warnings.append("sce_sys/pfs-version.dat not found")


def validate_source_match(
    fh: BinaryIO,
    header: ParsedHeader,
    inodes: list[ParsedInode],
    file_inodes: dict[str, int],
    source: Path,
    errors: list[str],
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
    progress: Progress | None = None,
) -> None:
    if not source.exists() or not source.is_dir():
        errors.append(f"source path does not exist or is not a directory: {source}")
        return

    source_files = sorted(p for p in source.rglob("*") if p.is_file())
    source_rel = {p.relative_to(source).as_posix() for p in source_files}
    image_rel = set(file_inodes.keys())

    for rel in sorted(source_rel - image_rel):
        errors.append(f"missing in image: {rel}")
    for rel in sorted(image_rel - source_rel):
        errors.append(f"extra in image: {rel}")

    common = sorted(source_rel & image_rel)
    # Report progress against the total logical bytes to compare, throttled by volume.
    total_bytes: int = sum(max(0, inodes[file_inodes[rel]].logical_size) for rel in common)
    progress_total: int = max(total_bytes, 1)
    processed: int = 0
    last_reported: int = 0
    update_interval: int = 8 * 1024 * 1024
    if progress is not None:
        progress.step("compare", 0, progress_total, bytes_processed=0)

    for rel in common:
        inode = inodes[file_inodes[rel]]
        # Hash the decoded image payload and the source file by streaming both.
        image_hash = hashlib.sha256()
        try:
            for chunk in iter_inode_logical_blocks(fh, header, inode, ekpfs=ekpfs, new_crypt=new_crypt):
                image_hash.update(chunk)
                processed += len(chunk)
                if progress is not None and processed - last_reported >= update_interval:
                    last_reported = processed
                    progress.step("compare", min(processed, total_bytes), progress_total, bytes_processed=processed)
        except (ValueError, OSError) as exc:
            errors.append(f"file '{rel}' failed to read payload: {exc}")
            continue

        source_hash = hashlib.sha256()
        with (source / rel).open("rb") as src_fh:
            while True:
                src_chunk: bytes = src_fh.read(4 * 1024 * 1024)
                if not src_chunk:
                    break
                source_hash.update(src_chunk)

        if image_hash.digest() != source_hash.digest():
            errors.append(f"content mismatch for file: {rel}")

    if progress is not None:
        progress.step("compare", progress_total, progress_total, bytes_processed=total_bytes)

def _image_size_bytes(image: Path) -> int:
    """Return the size of a path on disk, or zero when unavailable."""
    try:
        return image.stat().st_size
    except OSError:
        return 0

def read_pfs_info(image: Path) -> PFSImageInfo:
    """Read lightweight metadata from a PFS image.

    Args:
        image: Input PFS image path.

    Returns:
        A structured summary containing the parsed header and any warnings or errors.
    """
    info = PFSImageInfo(image=image, size_bytes=_image_size_bytes(image))

    if not image.exists() or not image.is_file():
        info.errors.append(f"image path does not exist or is not a file: {image}")
        return info

    try:
        with image.open("rb") as fh:
            info.header = parse_image_header(fh)
    except (OSError, ValueError) as exc:
        info.errors.append(f"failed to read image header: {exc}")
        return info

    if info.header.magic != consts.PFS_MAGIC:
        info.errors.append(f"header magic mismatch: 0x{info.header.magic:016X} != 0x{consts.PFS_MAGIC:016X}")
    if info.header.block_size <= 0 or (info.header.block_size & (info.header.block_size - 1)) != 0:
        info.errors.append(f"invalid block size {info.header.block_size}")
    if info.header.readonly != 1:
        info.warnings.append(f"header readonly byte is {info.header.readonly}, expected 1")

    return info


def inspect_pfs_image(
    image: Path,
    source: Path | None = None,
    expected_crc32: int | None = None,
    expected_manifest_sha256: str | None = None,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> PFSImageInspection:
    """Inspect a PFS image and collect structural validation details.

    Args:
        image: Input PFS image path.
        source: Optional source tree to compare against.
        expected_crc32: Optional expected cumulative payload CRC32.
        expected_manifest_sha256: Optional expected manifest SHA256 digest.
        ekpfs: Optional EKPFS key material for encrypted images.
        new_crypt: When True, use the alternate newCrypt key derivation path.

    Returns:
        A detailed inspection report with parsed tree data, warnings, and errors.
    """
    inspection: PFSImageInspection = PFSImageInspection(image=image, size_bytes=_image_size_bytes(image))

    if not image.exists() or not image.is_file():
        inspection.errors.append(f"image path does not exist or is not a file: {image}")
        return inspection

    try:
        with image.open("rb") as fh:
            header: ParsedHeader = parse_image_header(fh)
            inspection.header = header

            try:
                inodes: list[ParsedInode] = parse_image_inodes(fh, header, ekpfs=ekpfs, new_crypt=new_crypt)
            except (OSError, ValueError) as exc:
                inspection.errors.append(f"failed to parse inode table: {exc}")
                return inspection

            inspection.inodes = inodes
            validate_inode_layout(header, inodes, inspection.errors, inspection.warnings)

            try:
                verify_signed_image_signatures(fh, header, inodes, inspection.errors, ekpfs=ekpfs, new_crypt=new_crypt)
            except (OSError, ValueError) as exc:
                inspection.errors.append(f"failed to verify image signatures: {exc}")

            try:
                (
                    inspection.uroot_inode,
                    inspection.fpt_map,
                    inspection.collision_map,
                    inspection.special_inodes,
                ) = parse_superroot_and_indexes(
                    fh, header, inodes, inspection.errors, ekpfs=ekpfs, new_crypt=new_crypt
                )
            except (OSError, ValueError) as exc:
                inspection.errors.append(f"failed to parse superroot and indexes: {exc}")
                return inspection

            if inspection.uroot_inode >= 0:
                try:
                    inspection.file_inodes, inspection.dir_inodes, inspection.dirents_by_inode = build_tree_from_uroot(
                        fh,
                        header,
                        inodes,
                        inspection.uroot_inode,
                        inspection.errors,
                        ekpfs=ekpfs,
                        new_crypt=new_crypt,
                    )
                except (OSError, ValueError) as exc:
                    inspection.errors.append(f"failed to build filesystem tree: {exc}")
                    return inspection

                case_insensitive: bool = bool(header.mode & consts.PFS_MODE_CASE_INSENSITIVE)
                expected_fpt: dict = build_expected_fpt(
                    inspection.file_inodes, inspection.dir_inodes, case_insensitive
                )

                validate_fpt_maps(inspection.fpt_map, inspection.collision_map, expected_fpt, inspection.errors)
                validate_ps5_checklist(
                    fh,
                    header,
                    inodes,
                    inspection.file_inodes,
                    inspection.warnings,
                    inspection.errors,
                    ekpfs=ekpfs,
                    new_crypt=new_crypt,
                )

                try:
                    (
                        inspection.checked_files,
                        inspection.data_crc32,
                        inspection.manifest_sha256,
                    ) = verify_file_payload_hashes(
                        fh,
                        header,
                        inodes,
                        inspection.file_inodes,
                        inspection.errors,
                        ekpfs=ekpfs,
                        new_crypt=new_crypt,
                    )
                except (OSError, ValueError) as exc:
                    inspection.errors.append(f"failed to verify file payload hashes: {exc}")

                if expected_crc32 is not None and inspection.data_crc32 != expected_crc32:
                    inspection.errors.append(
                        f"CRC32 mismatch: actual 0x{inspection.data_crc32:08X}, expected 0x{expected_crc32:08X}"
                    )
                if (
                    expected_manifest_sha256 is not None
                    and inspection.manifest_sha256.lower() != expected_manifest_sha256.lower()
                ):
                    inspection.errors.append(
                        "Manifest SHA256 mismatch: actual "
                        f"{inspection.manifest_sha256}, expected {expected_manifest_sha256.lower()}"
                    )

                reachable = (
                    set(inspection.file_inodes.values())
                    | set(inspection.dir_inodes.values())
                    | set(inspection.special_inodes)
                )
                orphan_inodes = sorted(inode.number for inode in inodes if inode.number not in reachable)
                if orphan_inodes:
                    inspection.errors.append(
                        "orphan inodes not reachable from filesystem tree: "
                        + ", ".join(str(value) for value in orphan_inodes[:20])
                        + (" ..." if len(orphan_inodes) > 20 else "")
                    )

                if source is not None:
                    validate_source_match(
                        fh,
                        header,
                        inodes,
                        inspection.file_inodes,
                        source,
                        inspection.errors,
                        ekpfs=ekpfs,
                        new_crypt=new_crypt,
                    )

                inspection.compressed_files = sum(
                    1 for inode_num in inspection.file_inodes.values() if inodes[inode_num].is_compressed
                )
                inspection.logical_file_bytes = sum(
                    max(0, inodes[inode_num].logical_size) for inode_num in inspection.file_inodes.values()
                )
                inspection.stored_file_bytes = sum(
                    max(0, inodes[inode_num].stored_size) for inode_num in inspection.file_inodes.values()
                )
    except (OSError, ValueError) as exc:
        inspection.errors.append(f"failed to inspect image: {exc}")

    return inspection


def analyze_pfs_image(image: Path, new_crypt: bool = False) -> PFSImageInspection:
    """Analyze a PFS image without comparing it to a source tree.

    Args:
        image: Input PFS image path.
        new_crypt: When True, use the alternate newCrypt key derivation path.

    Returns:
        A detailed inspection report.
    """
    return inspect_pfs_image(image=image, new_crypt=new_crypt)


def verify_pfs_image(
    image: Path,
    source: Path | None = None,
    expected_crc32: int | None = None,
    expected_manifest_sha256: str | None = None,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> PFSImageInspection:
    """Verify a PFS image against optional source and hash expectations.

    Args:
        image: Input PFS image path.
        source: Optional source tree to compare against.
        expected_crc32: Optional expected cumulative payload CRC32.
        expected_manifest_sha256: Optional expected manifest SHA256 digest.
        ekpfs: Optional EKPFS key material for encrypted images.
        new_crypt: When True, use the alternate newCrypt key derivation path.

    Returns:
        A detailed inspection report.
    """
    return inspect_pfs_image(
        image=image,
        source=source,
        expected_crc32=expected_crc32,
        expected_manifest_sha256=expected_manifest_sha256,
        ekpfs=ekpfs,
        new_crypt=new_crypt,
    )


def extract_pfs_image(
    image: Path,
    output_path: Path,
    progress: Progress | None = None,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> PFSExtractionResult:
    """Extract all logical files from a PFS image.

    Args:
        image: Input PFS image path.
        output_path: Destination directory for extracted files.
        progress: Optional progress reporter.
        ekpfs: Optional EKPFS key material for encrypted images.
        new_crypt: When True, use the alternate newCrypt key derivation path.

    Returns:
        A structured extraction result.
    """
    result: PFSExtractionResult = PFSExtractionResult(image=image, output_path=output_path, bytes_written=0)
    inspection: PFSImageInspection = inspect_pfs_image(image=image, ekpfs=ekpfs, new_crypt=new_crypt)
    result.warnings.extend(inspection.warnings)
    result.errors.extend(inspection.errors)

    if result.errors:
        return result
    if inspection.header is None:
        result.errors.append("image header is not available")
        return result
    if output_path.exists() and not output_path.is_dir():
        result.errors.append(f"output path exists and is not a directory: {output_path}")
        return result

    directory_targets: list[Path] = [
        output_path / Path(rel_dir)
        for rel_dir in sorted(inspection.dir_inodes.keys(), key=lambda value: (value.count("/"), value.lower(), value))
        if rel_dir != ""
    ]
    file_targets: list[tuple[str, Path, int]] = [
        (rel_path, output_path / Path(rel_path), inode_num)
        for rel_path, inode_num in sorted(inspection.file_inodes.items())
    ]

    for directory_target in directory_targets:
        if directory_target.exists() and not directory_target.is_dir():
            result.errors.append(f"output path conflicts with a file: {directory_target}")
    for _rel_path, file_target, _inode_num in file_targets:
        if file_target.exists():
            result.errors.append(f"output file already exists: {file_target}")

    if result.errors:
        return result

    output_path.mkdir(parents=True, exist_ok=True)

    if progress is not None:
        progress.status(f"\nExtracting {len(file_targets)} files to {output_path}...")

    try:
        with image.open("rb") as fh:
            for directory_target in directory_targets:
                if not directory_target.exists():
                    directory_target.mkdir(parents=True, exist_ok=False)
                    result.directories_created += 1

            total_files: int = len(file_targets)
            for index, (rel_path, file_target, inode_num) in enumerate(file_targets, start=1):
                inode: ParsedInode = inspection.inodes[inode_num]
                file_target.parent.mkdir(parents=True, exist_ok=True)
                # Stream logical blocks straight to disk to keep memory flat for large files.
                try:
                    with file_target.open("wb") as out_fh:
                        for chunk in iter_inode_logical_blocks(
                            fh, inspection.header, inode, ekpfs=ekpfs, new_crypt=new_crypt
                        ):
                            out_fh.write(chunk)
                            result.bytes_written += len(chunk)
                except ValueError as exc:
                    result.errors.append(f"failed to decode file '{rel_path}' payload: {exc}")
                    return result

                result.files_written += 1

                if progress is not None:
                    progress.step("extract", index, total_files, bytes_processed=result.bytes_written)
    except (OSError, ValueError) as exc:
        result.errors.append(f"failed to extract image: {exc}")

    return result