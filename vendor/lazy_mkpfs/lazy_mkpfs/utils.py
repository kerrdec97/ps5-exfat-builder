# pfs/utils.py
from __future__ import annotations
import json
import tempfile
from pathlib import Path
from typing import BinaryIO
from .types import BuildError
import json
import tempfile
from pathlib import Path
from typing import BinaryIO
import os
import sys
import tempfile
from pathlib import Path
from typing import BinaryIO

def human_readable_size(size: int) -> str:
    """Convert a byte count to a human-readable string.

    Args:
        size: Number of bytes.

    Returns:
        Human readable string using binary prefixes (KB, MB, ...).
    """
    s: float = float(size)
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if s < 1024.0:
            return f"{s:.2f} {unit}"
        s /= 1024.0
    return f"{s:.2f} PB"


def ceil_div(a: int, b: int) -> int:
    """Compute the integer ceiling of a / b.

    Args:
        a: Numerator.
        b: Denominator (must be positive).

    Returns:
        The smallest integer >= a / b.
    """
    result: int = (a + b - 1) // b
    return result


def is_power_of_two(v: int) -> bool:
    """Return True if ``v`` is a positive power of two.

    Args:
        v: Value to test.

    Returns:
        True when v is 1,2,4,8,...; False otherwise.
    """
    return v > 0 and (v & (v - 1)) == 0


def normalize_output_path(path_arg: str, desired_suffix: str, adjust: bool = True) -> tuple[Path, bool]:
    """Normalize an output path extension when automatic adjustment is enabled.

    Args:
        path_arg: Input path string provided by the user.
        desired_suffix: Desired output suffix, including the leading dot.
        adjust: When True, replace the current suffix when it does not match the
            desired suffix. When False, return the path unchanged.

    Returns:
        A tuple of ``(normalized_path, changed)`` where ``changed`` is True when
        the suffix was updated.
    """
    p: Path = Path(path_arg)
    if not adjust:
        return p, False
    if p.suffix.lower() == desired_suffix.lower():
        return p, False
    normalized: Path = p.with_suffix(desired_suffix)
    return normalized, True

def get_available_ram_bytes() -> int:
    """Return available RAM in bytes, or 0 if undetectable."""
    if sys.platform == "win32":
        try:
            import ctypes
            class MEMORYSTATUSEX(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
                ]
            mem = MEMORYSTATUSEX()
            mem.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(mem))
            return mem.ullAvailPhys
        except Exception:
            return 0
    else:
        try:
            with open('/proc/meminfo', 'r') as f:
                for line in f:
                    if line.startswith('MemAvailable:'):
                        return int(line.split()[1]) * 1024
        except FileNotFoundError:
            pass
        try:
            pages = os.sysconf('SC_AVPHYS_PAGES')
            page_size = os.sysconf('SC_PAGE_SIZE')
            if pages > 0 and page_size > 0:
                return pages * page_size
        except (AttributeError, ValueError):
            pass
    return 0


def resolve_temp_root(temp_folder: Path | None = None, output_path: Path | None = None) -> Path:
    """Resolve the temporary root directory used for pack artifacts.

    Args:
        temp_folder: Optional caller-provided temp directory path.
        output_path: Optional output file path, used to default the temp folder
                     to a hidden directory next to the output file.

    Returns:
        Existing directory path used for temporary files.
    """
    # 1. If a custom temp folder is provided, use it
    if temp_folder is not None:
        temp_root: Path = temp_folder.expanduser().resolve()
        temp_root.mkdir(parents=True, exist_ok=True)
        return temp_root
    
    # 2. If no temp folder is provided, but we know the output path, 
    #    create a hidden .mkpfs_temp folder right next to the output file
    if output_path is not None:
        temp_root = output_path.parent / ".mkpfs_temp"
        temp_root.mkdir(parents=True, exist_ok=True)
        return temp_root

    # 3. Fallback to the system's default temporary directory
    return Path(tempfile.gettempdir())


def read_param_json(path: Path) -> dict[str, object]:
    """Read and parse a JSON parameter file used by games.

    Args:
        path: Path to the JSON file.

    Returns:
        Parsed JSON object as a dict.

    Raises:
        ValueError: When the file cannot be read or parsed as JSON.
    """
    try:
        with path.open(mode="r", encoding="utf-8") as f:
            result: dict[str, object] = json.load(f)
            return result
    except (OSError, json.JSONDecodeError) as exc:  # pragma: no cover - bubble up
        raise ValueError(f"Failed to parse {path}: {exc}") from exc


def _read_exact(fh: BinaryIO, offset: int, size: int) -> bytes:
    """Read exactly ``size`` bytes from file handle starting at ``offset``.

    Args:
        fh: Binary file-like object supporting seek and read.
        offset: Offset in bytes from the start of the file where read begins.
        size: Number of bytes to read.

    Returns:
        The requested bytes.

    Raises:
        ValueError: If the read returns fewer than ``size`` bytes.
    """
    fh.seek(offset)
    data: bytes = fh.read(size)
    if len(data) != size:
        raise ValueError(f"truncated read at offset {offset} (wanted {size}, got {len(data)})")
    return data