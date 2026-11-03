from __future__ import annotations
import re
from pathlib import Path
from .build import build_pfs, build_pfs_stream_single_file
from .create_exfat import create_exfat_image
from .ampr_index import ensure_ampr_index
from .types import BuildStats

def sanitize_output_path(output_path: Path, default_ext: str, max_len: int = 60) -> Path:
    if not output_path.suffix: 
        output_path = output_path.with_suffix(default_ext)
    if len(output_path.name) > max_len:
        ext_len = len(output_path.suffix)
        max_stem_len = max(1, max_len - ext_len)
        new_name = output_path.stem[:max_stem_len] + output_path.suffix
        output_path = output_path.with_name(new_name)
    return output_path

def extract_title_id(path_str: str) -> str | None:
    match = re.search(r'(CUSA|PPSA|PCAS|PCJS|PCES|NPXS)\d{5}', path_str, re.IGNORECASE)
    if match: 
        return match.group(0).upper()
    return None

def find_game_root(source: Path) -> Path | None:
    """Recursively search downwards for the folder containing eboot.bin."""
    try:
        # 1. Check if current directory contains eboot.bin (case-insensitive)
        for item in source.iterdir():
            if item.is_file() and item.name.lower() == "eboot.bin":
                return source
    except PermissionError:
        return None

    # 2. If not found, search in subdirectories
    try:
        for item in source.iterdir():
            if item.is_dir():
                result = find_game_root(item)
                if result:
                    return result
    except PermissionError:
        pass
        
    return None

def pack_folder(
    source_folder: str | Path,
    output_image: str | Path | None = None,
    block_size: int = 0x10000,
    pfs_version: int = 2,
    inode_bits: int = 32,
    case_insensitive: bool = True,
    compress: bool = True,
    zlib_level: int = 6,
    min_compress_size_mb: float = 5.0,
    cpu_count: int = 0,
    use_ram_if_possible: bool = True,
    zlib_backend: str = "zlib",
    verbose: bool = True,
    exfat: bool = True,
) -> BuildStats:
    source = Path(source_folder).resolve()
    if not source.is_dir():
        raise FileNotFoundError(f"Source folder does not exist or is not a directory: {source}")

    # ─────────────────────────────────────────────────────────────
    # AUTO-DETECT GAME ROOT
    # ─────────────────────────────────────────────────────────────
    actual_game_root = find_game_root(source)
    if actual_game_root is None:
        raise FileNotFoundError(
            f"Could not find 'eboot.bin' in '{source.name}' or any of its subdirectories.\n"
            "Please ensure you are pointing to a valid PS5 game folder."
        )
    
    if actual_game_root != source:
        if verbose:
            print(f"🔍 Auto-detected game root: '{actual_game_root.name}' (inside '{source.name}')")
        source = actual_game_root
    # ─────────────────────────────────────────────────────────────

    ensure_ampr_index(source)

    if output_image is None:
        default_ext = ".ffpfsc" if exfat else ".ffpfs"
        output = source.parent / f"{source.name}{default_ext}"
    else:
        output = Path(output_image).resolve()

    if exfat:
        output = sanitize_output_path(output, default_ext=".ffpfsc", max_len=60)
        
        # extract_title_id will still work perfectly because `str(source)` 
        # includes the parent folder path (e.g. C:\...\PPSA08576\Asterix...)
        title_id = extract_title_id(str(source))
        temp_exfat_name = f"{title_id}.exfat" if title_id else "pfs_image.exfat"
        temp_exfat = output.parent / temp_exfat_name
        
        try:
            if verbose:
                print("⚠️ Using exFAT Wrapper method for maximum PS5 compatibility...")
                print(f"   Pass 1: Creating temporary exFAT image {temp_exfat.name}...")
            create_exfat_image(source, temp_exfat, verbose=verbose)
            if verbose: 
                print(f"   Pass 2: Wrapping {temp_exfat.name} into compressed {output.name}...")
            
            stats = build_pfs_stream_single_file(
                source_file=temp_exfat, output_path=output, block_size=block_size, pfs_version=pfs_version,
                case_insensitive=case_insensitive, zlib_level=zlib_level, threshold_gain=1, min_file_gain=0,
                min_compress_size_mb=0.0, cpu_count=cpu_count, compress=True, encrypted=False,
                skip_executable_compression=False, dry_run=False, verbose=verbose,
                use_ram_if_possible=use_ram_if_possible, zlib_backend=zlib_backend,
            )
            return stats
        finally:
            if temp_exfat.exists():
                temp_exfat.unlink()
                if verbose: 
                    print(f"   Cleanup: Removed temporary {temp_exfat.name}")
    else:
        output = sanitize_output_path(output, default_ext=".ffpfs", max_len=60)
        if verbose: 
            print("⚠️ Using standard Folder packing method (.ffpfs)...")
        
        return build_pfs(
            source_root=source, output_path=output, block_size=block_size, pfs_version=pfs_version,
            inode_bits=inode_bits, case_insensitive=case_insensitive, signed=False, compress=compress,
            threshold_gain=5, cpu_count=cpu_count, zlib_level=zlib_level, dry_run=False, verbose=verbose,
            encrypted=False, min_file_gain=0, skip_executable_compression=True,
            min_compress_size_mb=min_compress_size_mb, use_ram_if_possible=use_ram_if_possible,
            zlib_backend=zlib_backend,
        )