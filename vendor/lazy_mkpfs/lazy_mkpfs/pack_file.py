from __future__ import annotations
from pathlib import Path
from .build import build_pfs_stream_single_file
from .types import BuildStats

def sanitize_output_path(output_path: Path, default_ext: str, max_len: int = 60) -> Path:
    if not output_path.suffix: output_path = output_path.with_suffix(default_ext)
    if len(output_path.name) > max_len:
        ext_len = len(output_path.suffix)
        max_stem_len = max(1, max_len - ext_len)
        new_name = output_path.stem[:max_stem_len] + output_path.suffix
        output_path = output_path.with_name(new_name)
    return output_path

def pack_file(
    source_file: str | Path,
    output_image: str | Path | None = None,
    block_size: int = 0x10000,
    pfs_version: int = 2,
    case_insensitive: bool = True,
    compress: bool = True,
    zlib_level: int = 6,
    min_compress_size_mb: float = 1.0,
    cpu_count: int = 0,
    use_ram_if_possible: bool = True,
    zlib_backend: str = "zlib",  # UPDATED
    verbose: bool = True,
) -> BuildStats:
    source = Path(source_file).resolve()
    if not source.is_file(): raise FileNotFoundError(f"Source file does not exist or is not a file: {source}")
    if output_image is None: output = source.parent / f"{source.stem}.ffpfsc"
    else: output = Path(output_image).resolve()

    output = sanitize_output_path(output, default_ext=".ffpfsc", max_len=60)

    return build_pfs_stream_single_file(
        source_file=source, output_path=output, block_size=block_size, pfs_version=pfs_version,
        case_insensitive=case_insensitive, zlib_level=zlib_level, threshold_gain=1, min_file_gain=0,
        min_compress_size_mb=min_compress_size_mb, cpu_count=cpu_count, compress=compress, encrypted=False,
        skip_executable_compression=False, dry_run=False, verbose=verbose,
        use_ram_if_possible=use_ram_if_possible, zlib_backend=zlib_backend,  # UPDATED
    )