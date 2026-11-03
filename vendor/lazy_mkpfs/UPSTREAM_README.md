# Lazy_MkPFS

A fast, automated, and user-friendly PS5 PFS image builder.

> **Disclaimer:** This is a personal project and is not intended to be a replacement for the original tool. It is built as a customized, streamlined, and optimized version designed for ease of use, speed, and seamless integration into other Python projects.
> 
> **Note on macOS:** macOS support is currently implemented in the codebase but has not been officially tested yet. Use at your own risk.

## Based On
**[MkPFS by PSBrew](https://github.com/PSBrew/MkPFS)**

## Key Upgrades and Features

Compared to the original project, Lazy_MkPFS includes several major quality-of-life and performance upgrades:

- **Massively Faster Performance:** Optimized Linux directory scanning (`os.scandir`), direct streaming compression (no temporary spool files for the final image), and native OS-pipe IPC for multiprocessing.
- **Advanced Compression Backends:** Added support for `zlib-ng` and Intel ISA-L (`zlib-isa`) for significantly faster compression speeds.
- **Cross-Platform exFAT Creator:** Automatically generates and wraps exFAT images for maximum PS5 compatibility.
- **Windows Specific Improvements:** Utilizes `OSFMount` for raw image mounting, executes the Windows `format` command silently in the background, and implements an automatic retry loop during unmounting to prevent "Access Denied" errors caused by Windows Explorer or Antivirus locking the volume.
- **Low RAM Verification:** Streams and hashes files block-by-block during verification, preventing `MemoryError` crashes on massive images.
- **Batch Processing:** Easily pack an entire folder of games concurrently with a single command and clean multi-progress tracking.
- **Auto-Dependency Installation:** Automatically detects and installs missing core dependencies (like `cryptography`) on the first run.
- **Modular and Importable:** Cleanly packaged as a `lazy_mkpfs` module, making it trivial to import and use the packing logic in your own Python scripts.

---

## Installation

Clone the repository and run any script. The tool will automatically install required core dependencies (like `cryptography`) on the first run.

```bash
git clone https://github.com/Nazky/Lazy_MkPFS.git
cd Lazy_MkPFS
```

## Terminal usage (CLI)

#### 1. Pack a Game Folder

```bash
# Pack a folder using Intel ISA-L (fastest) with the exFAT wrapper (Default)
python main_folder.py "./Games/PPSA01474" "./Output/RAC.ffpfsc" --zlib-isa --zlib-level 3

# Pack a folder using zlib-ng WITHOUT the exFAT wrapper (creates standard .ffpfs)
python main_folder.py "./Games/PPSA01474" "./Output/RAC.ffpfs" --zlib-ng --no-exfat
```

#### 2. Pack a Single File

```bash
# Pack a single .exfat or .ffpkg file into a compressed .ffpfsc image
python main_file.py "./game.exfat" "./game.ffpfsc"
```

#### 3. Batch Pack Multiple Games

```bash
# Pack all games inside a directory concurrently
python main_batch.py "./Games" "./Output" --zlib-ng --zlib-level 5 --workers 4
```

#### 4. Verify an Image

```bash
# Verify the integrity and hashes of a packed image
python main_verify.py "./Output/RAC.ffpfsc"
```

### CLI Arguments Reference

| Argument  | Applies To | Description |
|-------|-----|------------|
| source_folder / input_folder | folder, batch  | Source directory containing the game or games.   |
| source_file    | file  | Source file to pack (.exfat or .ffpkg).   |
| output_image / output_folder  | all  | Output path for the image or batch results. |
| --zlib-level [1-9] | folder, file, batch  | Zlib compression level (default: 6). |
| --zlib-ng | folder, file, batch  | Use the zlib-ng backend for faster compression. |
| --zlib-isa | folder, file, batch  | Use the Intel ISA-L backend (fastest). |
| --min-compress-size [MB]  | folder, file  | Minimum file size in MB to attempt compression. |
| --no-exfat  | folder, batch  | Disable exFAT wrapper (creates .ffpfs instead of .ffpfsc). |
| --cpu-count [N]  | folder, file, batch  | Number of CPU cores to use (0 = auto). |
| --workers [N]  | batch  | Number of parallel workers for batch processing. |
| --no-ram  | folder, file, batch  | Disable RAM-based writing (forces disk spooling). |
| --ekpfs [HEX] | verify  | EKPFS key in hex (for encrypted images). |
| --new-crypt  | verify  | Use alternate newCrypt key derivation. |
| --verbose, -v  | all  | Enable verbose output. |
| --quiet, -q  | all  | Suppress non-essential output. |

## Python API (Importing)

Because **Lazy_MkPFS** is fully modular, you can easily import its core functions into your own Python projects.

```python
from lazy_mkpfs import pack_folder, pack_file, pack_batch, verify_pfs
from pathlib import Path

# ==========================================
# 1. Pack a Folder
# ==========================================
stats = pack_folder(
    source_folder="./Games/PPSA01474",
    output_image="./Output/RAC.ffpfsc",
    zlib_backend="zlib",  # Options: "zlib", "zlib-ng", "isa-l"
    zlib_level=3,
    exfat=True,            # Creates the exFAT wrapper automatically
    verbose=True
)
print(f"Completed in {stats.elapsed_seconds:.2f}s | Gain: {stats.actual_gain_pct:.2f}%")

# ==========================================
# 2. Pack a Single File
# ==========================================
stats = pack_file(
    source_file="./game.exfat",
    output_image="./game.ffpfsc",
    zlib_backend="zlib-ng",
    zlib_level=6,
    compress=True
)

# ==========================================
# 3. Batch Pack Multiple Games
# ==========================================
results = pack_batch(
    input_dir="./Games",
    output_dir="./Output",
    workers=4,             # Number of parallel workers
    zlib_backend="isa-l",
    zlib_level=3,
    exfat=True             # Apply exFAT wrapper to folders
)
print(f"Success: {results['succeeded']} | Failed: {results['failed']}")

# ==========================================
# 4. Verify an Image
# ==========================================
verify_pfs(
    image="./Output/RAC.ffpfsc",
    verbose=True
)
```

---

## Credits

- **PSBrew** for the original [MkPFS](https://github.com/PSBrew/MkPFS) project and PFS format research.
- **pycompression** for the amazing compression libraries:
  - [python-isal](https://github.com/pycompression/python-isal) (Intel ISA-L)
  - [python-zlib-ng](https://github.com/pycompression/python-zlib-ng)
- [**Declan Kerr**](https://x.com/kerrdec97) for the windows testing.

---

## TODO

- [ ] Clean the code
- [ ] Fix MacOS support
- [ ] Make a GUI (peoples love GUI for some reasons)
- [ ] Test with big games (this project only have been tested with small games, from 3gb to 50gb)