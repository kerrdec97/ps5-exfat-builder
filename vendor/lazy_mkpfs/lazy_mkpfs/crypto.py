# pfs/crypto.py
from __future__ import annotations
import hashlib
import hmac
import struct
from typing import BinaryIO
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from . import consts
from .types import BuildError, ParsedHeader
from .utils import _read_exact, ceil_div

def pfs_gen_sign_key(ekpfs: bytes, seed: bytes) -> bytes:
    """Generate the HMAC-based signing key used for PFS signatures.

    This is a small wrapper around :func:`pfs_gen_crypto_key` that selects the
    conventionally reserved index for the signing key.

    Args:
        ekpfs: Master EKPFS key material.
        seed: PFS seed value from the image header.

    Returns:
        32-byte HMAC-SHA256-derived key.
    """
    return pfs_gen_crypto_key(ekpfs, seed, 2)

def hmac_sha256(key: bytes, data: bytes) -> bytes:
    """Return the HMAC-SHA256 digest of ``data`` using ``key``.

    Args:
        key: HMAC key.
        data: Data to authenticate.

    Returns:
        Raw 32-byte HMAC-SHA256 digest.
    """
    return hmac.new(key, data, hashlib.sha256).digest()


def pfs_gen_crypto_key(ekpfs: bytes, seed: bytes, index: int) -> bytes:
    """Derive a per-index cryptographic key with HMAC-SHA256.

    Args:
        ekpfs: Base key material.
        seed: Image seed bytes.
        index: Integer index distinguishing derived keys.

    Returns:
        32-byte derived key.
    """
    data: bytes = struct.pack("<I", index) + seed
    return hmac.new(ekpfs, data, hashlib.sha256).digest()


def resolve_ekpfs_key(ekpfs: bytes | None = None) -> bytes:
    """Return validated EKPFS key material, defaulting to the all-zero key.

    Args:
        ekpfs: Optional caller-provided EKPFS bytes.

    Returns:
        A validated 32-byte EKPFS key.

    Raises:
        BuildError: If the provided key is not exactly 32 bytes.
    """
    if ekpfs is None:
        return consts.ZERO_EKPFS
    if len(ekpfs) != len(consts.ZERO_EKPFS):
        raise BuildError(f"EKPFS key must be {len(consts.ZERO_EKPFS)} bytes, got {len(ekpfs)}")
    return ekpfs


def parse_ekpfs_key_hex(key_hex: str | None = None) -> bytes:
    """Parse a compact EKPFS hex string and default to the all-zero key.

    Args:
        key_hex: Optional 64-hex-character EKPFS string.

    Returns:
        Parsed EKPFS bytes, or the all-zero key when omitted.

    Raises:
        BuildError: If the provided text is not valid 64-character hex.
    """
    if key_hex is None:
        return consts.ZERO_EKPFS
    normalized_hex: str = key_hex.strip().lower()
    if normalized_hex == "":
        return consts.ZERO_EKPFS
    if len(normalized_hex) != 64 or any(char not in "0123456789abcdef" for char in normalized_hex):
        raise BuildError("--ekpfs-key must be exactly 64 hexadecimal characters")
    return bytes.fromhex(normalized_hex)


def pfs_gen_enc_keys(ekpfs: bytes, seed: bytes, new_crypt: bool = False) -> tuple[bytes, bytes]:
    """Derive XTS tweak and data keys for encrypted PFS images.

    Args:
        ekpfs: Master EKPFS key material.
        seed: PFS seed value from the image header.
        new_crypt: When True, derive the encryption key from HMAC(EKPFS, seed)
            before running the standard PFS key derivation.

    Returns:
        Tuple of `(tweak_key, data_key)`, each 16 bytes long.
    """
    base_key: bytes = (
        hmac_sha256(resolve_ekpfs_key(ekpfs=ekpfs), seed) if new_crypt else resolve_ekpfs_key(ekpfs=ekpfs)
    )
    enc_key: bytes = pfs_gen_crypto_key(base_key, seed, 1)
    tweak_key: bytes = enc_key[:16]
    data_key: bytes = enc_key[16:32]
    return tweak_key, data_key


def pfs_gen_xts_key(ekpfs: bytes, seed: bytes, new_crypt: bool = False) -> bytes:
    """Return the combined AES-XTS key bytes for the cryptography backend.

    Args:
        ekpfs: Master EKPFS key material.
        seed: PFS seed value from the image header.
        new_crypt: When True, use the alternate newCrypt derivation path.

    Returns:
        32-byte XTS key, ordered as data-key then tweak-key.
    """
    tweak_key: bytes
    data_key: bytes
    tweak_key, data_key = pfs_gen_enc_keys(ekpfs=ekpfs, seed=seed, new_crypt=new_crypt)
    return data_key + tweak_key


def pfs_xts_start_sector(block_size: int) -> int:
    """Return the first XTS sector index used for encrypted PFS blocks.

    Args:
        block_size: Filesystem block size in bytes.

    Returns:
        XTS sector index where block 1 begins.

    Raises:
        BuildError: If the block size is not a multiple of the XTS sector size.
    """
    if (block_size % consts.PFS_XTS_SECTOR_SIZE) != 0:
        raise BuildError(f"block size {block_size} is not aligned to XTS sector size {consts.PFS_XTS_SECTOR_SIZE}")
    return block_size // consts.PFS_XTS_SECTOR_SIZE


def pfs_xts_tweak(sector_number: int) -> bytes:
    """Return the 16-byte tweak input for one AES-XTS sector.

    Args:
        sector_number: Absolute XTS sector number.

    Returns:
        16-byte little-endian tweak buffer.
    """
    return struct.pack("<QQ", sector_number, 0)


def crypt_pfs_xts_sector(sector_data: bytes, xts_key: bytes, sector_number: int, *, encrypt: bool) -> bytes:
    """Encrypt or decrypt one AES-XTS sector.

    Args:
        sector_data: Sector bytes, must be exactly one XTS sector.
        xts_key: 32-byte XTS key in data+tweak order.
        sector_number: Absolute XTS sector number.
        encrypt: When True, encrypt; otherwise decrypt.

    Returns:
        Transformed sector bytes.

    Raises:
        BuildError: If the input is not exactly one XTS sector.
    """
    if len(sector_data) != consts.PFS_XTS_SECTOR_SIZE:
        raise BuildError(f"XTS sector input must be {consts.PFS_XTS_SECTOR_SIZE} bytes, got {len(sector_data)}")
    cipher: Cipher = Cipher(algorithm=algorithms.AES(xts_key), mode=modes.XTS(pfs_xts_tweak(sector_number)))
    transform = cipher.encryptor() if encrypt else cipher.decryptor()
    return transform.update(sector_data) + transform.finalize()


def read_image_bytes(
    fh: BinaryIO,
    header: ParsedHeader,
    offset: int,
    size: int,
    ekpfs: bytes | None = None,
    new_crypt: bool = False,
) -> bytes:
    """Read bytes from an image, transparently decrypting encrypted regions.

    Args:
        fh: Open image file handle.
        header: Parsed image header.
        offset: Absolute byte offset in the image.
        size: Number of bytes to read.
        ekpfs: Optional EKPFS key material. Defaults to the all-zero key.
        new_crypt: When True, use the alternate newCrypt key derivation path.

    Returns:
        Requested bytes, decrypted when the image is encrypted.

    Raises:
        ValueError: If the request spans plaintext header bytes and encrypted data.
    """
    if size <= 0:
        return b""
    if (header.mode & consts.PFS_MODE_ENCRYPTED) == 0 or offset < header.block_size:
        if offset < header.block_size < offset + size:
            raise ValueError("mixed plaintext/encrypted reads are not supported")
        return _read_exact(fh, offset, size)

    sector_size: int = consts.PFS_XTS_SECTOR_SIZE
    aligned_start: int = (offset // sector_size) * sector_size
    aligned_end: int = ceil_div(offset + size, sector_size) * sector_size
    raw: bytes = _read_exact(fh, aligned_start, aligned_end - aligned_start)
    xts_key: bytes = pfs_gen_xts_key(resolve_ekpfs_key(ekpfs=ekpfs), header.seed, new_crypt=new_crypt)
    decrypted = bytearray()
    sector_number: int = aligned_start // sector_size
    for chunk_offset in range(0, len(raw), sector_size):
        chunk: bytes = raw[chunk_offset : chunk_offset + sector_size]
        decrypted += crypt_pfs_xts_sector(chunk, xts_key, sector_number, encrypt=False)
        sector_number += 1
    inner_offset: int = offset - aligned_start
    return bytes(decrypted[inner_offset : inner_offset + size])


def encrypt_image_filesystem(
    out: BinaryIO,
    block_size: int,
    total_blocks: int,
    ekpfs: bytes,
    seed: bytes,
    new_crypt: bool = False,
    skip_block_numbers: set[int] | None = None,
) -> None:
    """Encrypt all on-disk filesystem sectors after the plaintext header block.

    Args:
        out: Open writable image file handle.
        block_size: Filesystem block size in bytes.
        total_blocks: Total number of blocks in the image.
        ekpfs: EKPFS key material.
        seed: PFS seed value from the image header.
        new_crypt: When True, use the alternate newCrypt key derivation path.
        skip_block_numbers: Optional filesystem block numbers that must remain
            plaintext and must not be XTS-encrypted.
    """
    xts_key: bytes = pfs_gen_xts_key(resolve_ekpfs_key(ekpfs=ekpfs), seed, new_crypt=new_crypt)
    start_sector: int = pfs_xts_start_sector(block_size=block_size)
    total_sectors: int = (total_blocks * block_size) // consts.PFS_XTS_SECTOR_SIZE
    skipped_blocks: set[int] = skip_block_numbers or set()
    sector_buffer: bytes
    for sector_number in range(start_sector, total_sectors):
        if (sector_number * consts.PFS_XTS_SECTOR_SIZE) // block_size in skipped_blocks:
            continue
        sector_offset: int = sector_number * consts.PFS_XTS_SECTOR_SIZE
        sector_buffer = _read_exact(out, sector_offset, consts.PFS_XTS_SECTOR_SIZE)
        out.seek(sector_offset)
        out.write(crypt_pfs_xts_sector(sector_buffer, xts_key, sector_number, encrypt=True))

def block_hmac_without_slot(block_data: bytes, sig_offset_in_block: int, size: int, signed: bool = True) -> bytes:
    chunk = bytearray(block_data[:size])
    if signed and 0 <= sig_offset_in_block <= len(chunk) - consts.SIG_SIZE:
        chunk[sig_offset_in_block : sig_offset_in_block + consts.SIG_SIZE] = b"\x00" * consts.SIG_SIZE
    return bytes(chunk)