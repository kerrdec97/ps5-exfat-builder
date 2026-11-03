# pfs/types.py
from __future__ import annotations
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol
import struct
from . import consts

@dataclass(frozen=True)
class SignedInodeLayout:
    """Describe the signed inode layout currently being written or parsed.

    Attributes:
        inode_size: Total serialized inode size.
        entry_size: Size of each signed block entry, including signature and pointer.
        block_format: ``struct`` format string for the on-disk block pointer.
        pointer_table_offset: Offset where the signed db/ib entry table begins.
    """

    inode_size: int
    entry_size: int
    block_format: str
    pointer_table_offset: int

class BuildError(RuntimeError):
    pass

@dataclass
class SignatureTarget:
    block: int
    sig_offset: int
    size: int
    level: int

@dataclass
class Dirent:
    inode_number: int
    type_code: int
    name: str

    @property
    def name_length(self) -> int:
        return len(self.name)

    @property
    def ent_size(self) -> int:
        size = self.name_length + 17
        rem = size % 8
        if rem:
            size += 8 - rem
        return size

    def to_bytes(self) -> bytes:
        """Serialize this directory entry to the on-disk dirent format.

        The returned bytes contain fields (inode, type, name length, entry size)
        followed by the ASCII name and padding to reach the aligned entry size.

        Returns:
            Bytes suitable for writing into a directory payload block.

        Raises:
            ValueError: If the name contains non-ASCII characters.
        """
        if not self.name.isascii():
            raise ValueError(
                f"Filename {self.name!r} contains non-ASCII characters and cannot be stored in a PFS image"
            )
        name_bytes: bytes = self.name.encode("ascii")
        out: bytearray = bytearray()
        out += struct.pack("<Iiii", self.inode_number, self.type_code, self.name_length, self.ent_size)
        out += name_bytes
        if len(out) < self.ent_size:
            out += b"\x00" * (self.ent_size - len(out))
        return bytes(out)

@dataclass
class Inode:
    number: int
    mode: int
    nlink: int
    flags: int
    size: int
    size_compressed: int
    blocks: int
    db: list[int] = field(default_factory=lambda: [0] * consts.MAX_DIRECT_BLOCKS)
    ib: list[int] = field(default_factory=lambda: [0] * consts.MAX_INDIRECT_BLOCKS)
    db_sig: list[bytes] = field(
        default_factory=lambda: [b"\x00" * consts.SIG_SIZE for _ in range(consts.MAX_DIRECT_BLOCKS)]
    )
    ib_sig: list[bytes] = field(
        default_factory=lambda: [b"\x00" * consts.SIG_SIZE for _ in range(consts.MAX_INDIRECT_BLOCKS)]
    )
    time_sec: int = 0

    def _base_bytes(self) -> bytearray:
        """Return common inode header bytes used by various on-disk inode layouts.

        This helper centralizes packing of the fixed-size fields present in both
        signed and unsigned inode representations.
        """
        ts: int = self.time_sec
        time_nsec: int = 0
        uid: int = 0
        gid: int = 0
        unk1: int = 0
        unk2: int = 0
        out: bytearray = bytearray()
        out += struct.pack("<HHI", self.mode, self.nlink, self.flags)
        out += struct.pack("<qq", self.size, self.size_compressed)
        out += struct.pack("<qqqq", ts, ts, ts, ts)
        out += struct.pack("<IIII", time_nsec, time_nsec, time_nsec, time_nsec)
        out += struct.pack("<IIQQI", uid, gid, unk1, unk2, self.blocks)
        return out

    def to_bytes(self) -> bytes:
        """Serialize the inode in the unsigned D32 layout.

        Returns:
            Bytes of length INODE_D32_SIZE containing the inode fields.

        Raises:
            BuildError: If the produced byte length does not match expectations.
        """
        out: bytearray = self._base_bytes()
        out += struct.pack("<" + "i" * consts.MAX_DIRECT_BLOCKS, *self.db)
        out += struct.pack("<" + "i" * consts.MAX_INDIRECT_BLOCKS, *self.ib)
        if len(out) != consts.INODE_D32_SIZE:
            raise BuildError(f"Unexpected inode size {len(out)}")
        return bytes(out)

    def to_bytes_signed32(self) -> bytes:
        """Serialize the inode using the signed S32 layout (32-byte signatures).

        This layout interleaves 32-byte signature placeholders and 4-byte block
        pointers for each direct/indirect entry.
        """
        return self._to_bytes_signed(layout=signed_inode_layout(32))

    def to_bytes_signed64(self) -> bytes:
        """Serialize the inode using the signed S64 layout (32-byte signatures).

        This layout stores the same signatures as S32, but uses 64-bit block
        pointers and includes the observed 4-byte padding after the ``blocks``
        field before the pointer table begins.
        """
        return self._to_bytes_signed(layout=signed_inode_layout(64))

    def _to_bytes_signed(self, *, layout: SignedInodeLayout) -> bytes:
        """Serialize a signed inode using the supplied signed layout metadata.

        Args:
            layout: Signed inode layout description.

        Returns:
            Serialized inode bytes for the selected signed layout.

        Raises:
            BuildError: If signature size or final inode size is invalid.
        """
        out: bytearray = self._base_bytes()
        if len(out) < layout.pointer_table_offset:
            out += b"\x00" * (layout.pointer_table_offset - len(out))
        for sig, block in zip(self.db_sig, self.db):
            if len(sig) != consts.SIG_SIZE:
                raise BuildError("Signed inode direct signature must be 32 bytes")
            out += sig
            out += struct.pack(layout.block_format, block)
        for sig, block in zip(self.ib_sig, self.ib):
            if len(sig) != consts.SIG_SIZE:
                raise BuildError("Signed inode indirect signature must be 32 bytes")
            out += sig
            out += struct.pack(layout.block_format, block)
        if len(out) != layout.inode_size:
            raise BuildError(f"Unexpected signed inode size {len(out)}")
        return bytes(out)

@dataclass
class FileNode:
    rel_path: str
    abs_path: Path
    parent_rel_dir: str
    name: str
    raw_size: int
    stored_source_path: Path | None = None
    stored_source_is_temp: bool = False
    stored_size: int = 0
    compressed: bool = False
    gain_pct: float = 0.0
    hypothetical_compressed_size: int = 0
    inode: Inode | None = None

def signed_inode_layout(inode_bits: int) -> SignedInodeLayout:
    """Return the signed inode layout for the requested inode width.

    Args:
        inode_bits: Signed inode width, 32 or 64.

    Returns:
        Layout metadata for the signed inode structure.

    Raises:
        BuildError: If ``inode_bits`` is not a supported signed width.
    """
    if inode_bits == 32:
        return SignedInodeLayout(
            inode_size=consts.INODE_S32_SIZE,
            entry_size=consts.SIG_ENTRY_S32_SIZE,
            block_format="<i",
            pointer_table_offset=0x64,
        )
    if inode_bits == 64:
        return SignedInodeLayout(
            inode_size=consts.INODE_S64_SIZE,
            entry_size=consts.SIG_ENTRY_S64_SIZE,
            block_format="<q",
            pointer_table_offset=0x68,
        )
    raise BuildError(f"Unsupported signed inode width: {inode_bits}")

@dataclass
class DirNode:
    rel_dir: str
    name: str
    parent_rel_dir: str | None
    children_dirs: list[str] = field(default_factory=list)
    children_files: list[str] = field(default_factory=list)
    dirents: list[Dirent] = field(default_factory=list)
    inode: Inode | None = None

@dataclass
class BuildStats:
    input_path: Path
    output_path: Path
    total_files: int = 0
    uncompressed_total_size: int = 0
    stored_total_size: int = 0
    all_compressed_total_size: int = 0
    compressed_files: int = 0
    uncompressed_files: int = 0
    elapsed_seconds: float = 0.0
    compression_enabled: bool = True
    block_size: int = 65536
    block_alignment_waste: int = 0

    @property
    def actual_gain_pct(self) -> float:
        if self.uncompressed_total_size == 0:
            return 0.0
        return ((self.uncompressed_total_size - self.stored_total_size) / self.uncompressed_total_size) * 100.0

    @property
    def max_possible_gain_pct(self) -> float:
        if self.uncompressed_total_size == 0:
            return 0.0
        return ((self.uncompressed_total_size - self.all_compressed_total_size) / self.uncompressed_total_size) * 100.0
    
@dataclass
class PFSCHeader:
    """PFSC header compatible with the reference ``PFSCHdr`` layout.

    Args:
        magic: PFSC magic value.
        unk4: Expected zero field at offset ``0x04``.
        unk8: Observed version-like field at offset ``0x08``.
        logical_block_size: Logical PFSC block size.
        block_offsets_offset: Offset to the block offset table from payload start.
        data_offset: Absolute offset where PFSC block data begins.
        data_length: Logical padded byte length managed by the PFSC stream.
    """

    magic: int
    unk4: int
    unk8: int
    logical_block_size: int
    block_offsets_offset: int
    data_offset: int
    data_length: int

@dataclass
class ParsedHeader:
    version: int
    magic: int
    mode: int
    block_size: int
    nblock: int
    dinode_count: int
    ndblock: int
    dinode_block_count: int
    readonly: int
    seed: bytes


@dataclass
class ParsedInode:
    number: int
    mode: int
    nlink: int
    flags: int
    size: int
    size_compressed: int
    blocks: int
    db: list[int]
    ib: list[int]
    db_sig: list[bytes] = field(default_factory=list)
    ib_sig: list[bytes] = field(default_factory=list)

    @property
    def is_dir(self) -> bool:
        return (self.mode & consts.INODE_MODE_DIR) != 0

    @property
    def is_file(self) -> bool:
        return (self.mode & consts.INODE_MODE_FILE) != 0

    @property
    def is_compressed(self) -> bool:
        return (self.flags & consts.INODE_FLAG_COMPRESSED) != 0

    @property
    def stored_size(self) -> int:
        return self.size if self.is_compressed else self.size_compressed

    @property
    def logical_size(self) -> int:
        return self.size_compressed if self.is_compressed else self.size


@dataclass
class ParsedDirent:
    inode_number: int
    type_code: int
    name: str

@dataclass
class PFSOperationResult:
    """Base result object for high-level PFS operations.

    Args:
        image: Input image path.
        errors: Collected fatal or validation errors.
        warnings: Collected non-fatal warnings.
    """

    image: Path
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


@dataclass
class PFSImageInfo(PFSOperationResult):
    """Lightweight PFS image metadata summary.

    Args:
        image: Input image path.
        errors: Collected fatal or validation errors.
        warnings: Collected non-fatal warnings.
        size_bytes: Image size on disk.
        header: Parsed image header, when available.
    """

    size_bytes: int = 0
    header: ParsedHeader | None = None

    @property
    def version_label(self) -> str:
        """Return the human-friendly version label."""
        if self.header is None:
            return ""
        return "PS5" if self.header.version == consts.PFS_VERSION_PS5 else "PS4"


@dataclass
class PFSImageInspection(PFSImageInfo):
    """Detailed PFS image inspection result.

    Args:
        image: Input image path.
        errors: Collected fatal or validation errors.
        warnings: Collected non-fatal warnings.
        size_bytes: Image size on disk.
        header: Parsed image header, when available.
        inodes: Parsed inode table.
        uroot_inode: Inode number of the filesystem root.
        file_inodes: Mapping of relative file paths to inode numbers.
        dir_inodes: Mapping of relative directory paths to inode numbers.
        dirents_by_inode: Parsed directory entries for each inode.
        fpt_map: Parsed flat_path_table entries.
        collision_map: Parsed collision resolver entries.
        special_inodes: Inodes reserved by the filesystem layout.
        checked_files: Number of payload hashes checked.
        data_crc32: Cumulative CRC32 of logical file payloads.
        manifest_sha256: SHA256 digest of the logical file manifest.
        compressed_files: Number of files stored compressed.
        logical_file_bytes: Total logical file payload bytes.
        stored_file_bytes: Total stored file payload bytes.
    """

    inodes: list[ParsedInode] = field(default_factory=list)
    uroot_inode: int = -1
    file_inodes: dict[str, int] = field(default_factory=dict)
    dir_inodes: dict[str, int] = field(default_factory=dict)
    dirents_by_inode: dict[int, list[ParsedDirent]] = field(default_factory=dict)
    fpt_map: dict[int, int] = field(default_factory=dict)
    collision_map: dict[int, list[ParsedDirent]] = field(default_factory=dict)
    special_inodes: set[int] = field(default_factory=set)
    checked_files: int = 0
    data_crc32: int = 0
    manifest_sha256: str = ""
    compressed_files: int = 0
    logical_file_bytes: int = 0
    stored_file_bytes: int = 0

    @property
    def has_tree(self) -> bool:
        """Return whether the inspection contains a parsed filesystem tree."""
        return self.uroot_inode >= 0 and len(self.dirents_by_inode) > 0


@dataclass
class PFSExtractionResult(PFSOperationResult):
    """Result of extracting a PFS image to a directory.

    Args:
        image: Input image path.
        errors: Collected fatal or validation errors.
        warnings: Collected non-fatal warnings.
        output_path: Destination directory path.
        files_written: Number of files written to disk.
        directories_created: Number of directories created or ensured.
        bytes_written: Total logical file bytes written to disk.
    """

    output_path: Path | None = None
    files_written: int = 0
    directories_created: int = 0
    bytes_written: int = 0

class SupportsIntQueue(Protocol):
    """Protocol for queue-like objects used by compression progress reporting."""

    def put(self, item: int) -> None:
        """Push a byte delta into the queue."""

    def get_nowait(self) -> int:
        """Return the next queued byte delta without blocking."""