# pfs/create_exfat.py
from __future__ import annotations
import os
import re
import platform
import subprocess
import tempfile
import shutil
import math
import threading
import queue
import time
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Callable

# ─────────────────────────────────────────────
# Progress Bar (Python-native, thread-safe)
# ─────────────────────────────────────────────

class CopyProgressBar:
    def __init__(self, total_bytes: int, total_files: int, width: int = 40):
        self.total_bytes = total_bytes
        self.total_files = total_files
        self.width = width
        self.copied_bytes = 0
        self.copied_files = 0
        self._lock = threading.Lock()
        self._queue = queue.Queue()
        self._done = threading.Event()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._start_time = None

    def start(self):
        self._start_time = time.time()
        self._thread.start()

    def _reader(self):
        while not self._done.is_set() or not self._queue.empty():
            try:
                event = self._queue.get(timeout=0.1)
            except queue.Empty:
                continue
            if event is None:
                break
            bytes_delta, files_delta = event
            with self._lock:
                self.copied_bytes += bytes_delta
                self.copied_files += files_delta
                self._draw()

    def _draw(self):
        if self.total_bytes == 0:
            return
        pct = min(self.copied_bytes / self.total_bytes, 1.0)
        filled = int(self.width * pct)
        bar = "█" * filled + "░" * (self.width - filled)
        elapsed = time.time() - self._start_time
        speed = self.copied_bytes / elapsed if elapsed > 0 else 0
        speed_str = self._human_speed(speed)
        if speed > 0:
            remaining = (self.total_bytes - self.copied_bytes) / speed
            eta_str = self._human_time(remaining)
        else:
            eta_str = "--:--"
        line = (
            f"\r│{bar}│ {pct*100:5.1f}% "
            f"│ {self.copied_files}/{self.total_files} files "
            f"│ {self._human_size(self.copied_bytes)}/{self._human_size(self.total_bytes)} "
            f"│ {speed_str} "
            f"│ ETA {eta_str}"
        )
        print(line, end="", flush=True)

    @staticmethod
    def _human_size(size: float) -> str:
        for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
            if abs(size) < 1024.0:
                return f"{size:.1f}{unit}"
            size /= 1024.0
        return f"{size:.1f}PB"

    @staticmethod
    def _human_speed(bps: float) -> str:
        return f"{CopyProgressBar._human_size(bps)}/s"

    @staticmethod
    def _human_time(seconds: float) -> str:
        if seconds < 60:
            return f"{int(seconds)}s"
        elif seconds < 3600:
            return f"{int(seconds//60)}m{int(seconds%60)}s"
        else:
            return f"{int(seconds//3600)}h{int((seconds%3600)//60)}m"

    def update(self, bytes_delta: int, files_delta: int = 0):
        self._queue.put((bytes_delta, files_delta))

    def finish(self):
        self._done.set()
        self._queue.put(None)
        self._thread.join(timeout=2.0)
        with self._lock:
            self._draw()
        print()

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *args):
        self.finish()


# ─────────────────────────────────────────────
# Parallel File Copy with Source Protection
# ─────────────────────────────────────────────

def _copy_file_worker(
    src_dst_pair: tuple[Path, Path],
    progress_callback: Callable[[int, int], None],
    buffer_size: int = 4 * 1024 * 1024,
    report_interval: int = 32 * 1024 * 1024,
) -> None:
    src, dst = src_dst_pair
    dst.parent.mkdir(parents=True, exist_ok=True)
    file_size = src.stat().st_size

    # Kernel-side zero-copy: copy_file_range never touches Python userspace for data.
    # This is what cp and file managers use internally on Linux.
    if platform.system() == "Linux" and hasattr(os, "copy_file_range"):
        try:
            with open(src, "rb") as fsrc, open(dst, "wb") as fdst:
                src_fd = fsrc.fileno()
                dst_fd = fdst.fileno()
                offset = 0
                last_report = 0
                while offset < file_size:
                    chunk = min(128 * 1024 * 1024, file_size - offset)  # 128MB slices
                    sent = os.copy_file_range(src_fd, dst_fd, chunk, offset_dst=offset)
                    if sent == 0:
                        break
                    offset += sent
                    if offset - last_report >= report_interval:
                        progress_callback(offset - last_report, 0)
                        last_report = offset
                progress_callback(offset - last_report, 1)
            return
        except OSError:
            pass  # cross-device or unsupported, fall through

    # Fallback: chunked read/write (Windows, macOS, or if copy_file_range fails)
    with open(src, "rb") as fsrc, open(dst, "wb") as fdst:
        copied = 0
        last_report = 0
        while True:
            chunk = fsrc.read(buffer_size)
            if not chunk:
                break
            fdst.write(chunk)
            copied += len(chunk)
            if copied - last_report >= report_interval:
                progress_callback(copied - last_report, 0)
                last_report = copied
        progress_callback(copied - last_report, 1)


def parallel_copy_to_mount(
    source: Path,
    mount_point: Path,
    progress_bar: CopyProgressBar | None = None,
    max_workers: int = None,
) -> None:
    """
    Direct parallel copy into a mounted filesystem.
    """
    tasks = []
    total_bytes = 0

    for dirpath, dirnames, filenames in os.walk(source):
        rel_dir = Path(dirpath).relative_to(source)
        for dirname in dirnames:
            dst_dir = mount_point / rel_dir / dirname
            dst_dir.mkdir(parents=True, exist_ok=True)

        for filename in filenames:
            src_file = Path(dirpath) / filename
            if os.path.islink(src_file):
                continue
            dst_file = mount_point / rel_dir / filename
            tasks.append((src_file, dst_file))
            total_bytes += src_file.stat().st_size

    if not tasks:
        return

    if max_workers is None:
        cpu_count = os.cpu_count() or 2
        max_workers = min(cpu_count * 4, 16)

    def _progress(bytes_delta: int, files_delta: int):
        if progress_bar:
            progress_bar.update(bytes_delta, files_delta)

    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        futures = {
            executor.submit(_copy_file_worker, task, _progress): task
            for task in tasks
        }

        for future in as_completed(futures):
            try:
                future.result()
            except Exception as e:
                for f in futures:
                    f.cancel()
                raise RuntimeError(f"Copy failed for {futures[future]}: {e}")


def atomic_copy_with_protection(
    source: Path,
    destination: Path,
    staging_parent: Path,
    verbose: bool = True,
) -> None:
    """
    Copy source to destination with full protection.
    staging_parent MUST be on the host filesystem (not a mount point).
    """
    source = source.resolve()
    destination = destination.resolve()
    staging_parent = staging_parent.resolve()

    if not source.is_dir():
        raise FileNotFoundError(f"Source does not exist: {source}")

    # Verify the source directory is accessible (portable — replaces a Linux-only
    # O_PATH|O_DIRECTORY fd probe). Equivalent read-only intent on all platforms.
    if not (source.is_dir() and os.access(str(source), os.R_OK)):
        raise PermissionError(f"Cannot read source directory: {source}")

    total_bytes = 0
    total_files = 0
    for dirpath, _, filenames in os.walk(source):
        for f in filenames:
            fp = Path(dirpath) / f
            if not fp.is_symlink():
                total_bytes += fp.stat().st_size
                total_files += 1

    staging_parent.mkdir(parents=True, exist_ok=True)
    staging = tempfile.mkdtemp(prefix=".exfat_staging_", dir=str(staging_parent))

    try:
        if verbose:
            print(f"📝 Copying {total_files} files ({human_readable_size(total_bytes)})...")

        with CopyProgressBar(total_bytes, total_files) as bar:
            parallel_copy_to_mount(source, Path(staging), bar)

        if verbose:
            print("🔐 Finalizing (atomic rename)...")
        os.replace(staging, str(destination))

    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


# ─────────────────────────────────────────────
# Original Utility Functions
# ─────────────────────────────────────────────

def human_readable_size(size: int) -> str:
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if size < 1024.0:
            return f"{size:.2f} {unit}"
        size /= 1024.0
    return f"{size:.2f} PB"

def calculate_exfat_size(source: Path, force_cluster: int | None):
    cluster_size = force_cluster
    total_raw = 0
    total_alloc = 0
    file_count = 0
    dir_count = 0

    file_sizes = []
    for dirpath, dirnames, filenames in os.walk(source):
        dir_count += len(dirnames)
        for f in filenames:
            fp = os.path.join(dirpath, f)
            if not os.path.islink(fp):
                size = os.path.getsize(fp)
                total_raw += size
                file_sizes.append(size)
                file_count += 1

    if cluster_size is None:
        cluster_size = 65536

    for size in file_sizes:
        total_alloc += math.ceil(size / cluster_size) * cluster_size

    data_clusters = math.ceil(total_alloc / cluster_size)
    fat_bytes = data_clusters * 4
    bitmap_bytes = math.ceil(data_clusters / 8)
    entry_bytes = (file_count + dir_count) * 256
    meta_fixed = 32 * 1024 * 1024 

    base_total = total_alloc + fat_bytes + bitmap_bytes + entry_bytes + meta_fixed
    spare = max(base_total // 200, 64 * 1024 * 1024)
    spare = min(spare, 512 * 1024 * 1024)
    #spare = max(total_raw // 50, 256 * 1024 * 1024)

    total = base_total + spare
    min_total = total_raw + 64 * 1024 * 1024
    total = max(total, min_total)

    mb = math.ceil(total / (1024 * 1024))
    return mb, cluster_size, total_raw, file_count

def run_cmd(cmd, check=True, capture=True):
    if capture:
        return subprocess.run(cmd, check=check, capture_output=True, text=True)
    return subprocess.run(cmd, check=check)

# ==========================================
# LINUX IMPLEMENTATION
# ==========================================
def _linux_alloc_image(output_abs: Path, size_mb: int) -> None:
    size_bytes = size_mb * 1024 * 1024
    path_str = str(output_abs)

    if shutil.which("fallocate"):
        res = subprocess.run(
            ["fallocate", "-l", str(size_bytes), path_str],
            capture_output=True, text=True
        )
        if res.returncode == 0:
            return

    res = subprocess.run(
        ["truncate", "-s", str(size_bytes), path_str],
        capture_output=True, text=True
    )
    if res.returncode == 0:
        return

    print("   (fallocate and truncate unavailable; falling back to dd…)")
    block_mib = 64
    full_blocks, remainder = divmod(size_mb, block_mib)
    if full_blocks:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={path_str}",
             f"bs={block_mib}M", f"count={full_blocks}",
             "status=progress", "iflag=fullblock"],
            check=True
        )
    if remainder:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={path_str}",
             f"bs=1M", f"count={remainder}",
             f"seek={full_blocks}", "status=progress", "conv=notrunc"],
            check=True
        )


def _linux_setup_loop(output_abs: Path) -> str:
    res = subprocess.run(
        ["losetup", "--find", "--show", str(output_abs)],
        check=True, capture_output=True, text=True
    )
    loop_dev = res.stdout.strip()
    if not loop_dev:
        raise RuntimeError("losetup --find --show returned no device")
    return loop_dev


def _linux_copy_files(source: Path, mount_point: str, output_dir: Path) -> None:
    dst_path = Path(mount_point)

    total_bytes = 0
    total_files = 0
    for dirpath, _, filenames in os.walk(source):
        for f in filenames:
            fp = Path(dirpath) / f
            if not fp.is_symlink():
                total_bytes += fp.stat().st_size
                total_files += 1

    print(f"📝 Copying {total_files} files ({human_readable_size(total_bytes)})...")
    with CopyProgressBar(total_bytes, total_files) as bar:
        parallel_copy_to_mount(source, dst_path, bar, max_workers=4)


def create_image_linux(source, output, size_mb, cluster_size, label):
    cluster_arg = f"{cluster_size // 1024}K"
    output_abs = output.resolve()
    output_name = output_abs.name
    output_dir  = output_abs.parent

    # ── 1. Allocate ───────────────────────────────────────────────────────────
    print(f"\n📦 Creating raw image ({human_readable_size(size_mb * 1024 * 1024)})...")
    _linux_alloc_image(output_abs, size_mb)

    actual_size   = output_abs.stat().st_size
    expected_size = size_mb * 1024 * 1024
    if actual_size != expected_size:
        raise RuntimeError(
            f"Image size mismatch after allocation: "
            f"expected {expected_size}, got {actual_size}"
        )

    # ── 2. Format ─────────────────────────────────────────────────────────────
    print(f"🛠️  Formatting as exFAT (cluster={cluster_arg}, label='{label}')...")
    try:
        subprocess.run(
            ["mkfs.exfat", "-c", cluster_arg, "-L", label, output_name],
            check=True, cwd=str(output_dir),
            capture_output=True, text=True
        )
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"mkfs.exfat failed: {e.stderr or e.stdout}") from e

    # ── 3 & 4. Attach + mount (sudo losetup + sudo mount) ─────────────────────
    print("📂 Mounting image...")
    loop_dev = None
    mount_point = tempfile.mkdtemp(prefix="exfat_mount_")
    uid, gid = os.getuid(), os.getgid()

    # Attach loop device — try without sudo first, fall back to sudo
    for cmd in (
        ["losetup", "--find", "--show", str(output_abs)],
        ["sudo", "losetup", "--find", "--show", str(output_abs)],
    ):
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            loop_dev = res.stdout.strip()
            break

    if not loop_dev:
        raise RuntimeError(
            "losetup failed even with sudo.\n"
            "Run: sudo losetup --find --show <image>  to diagnose."
        )

    # Mount — try without sudo first, fall back to sudo
    mounted = False
    for cmd in (
        ["mount", "-o", f"uid={uid},gid={gid}", loop_dev, mount_point],
        ["sudo", "mount", "-o", f"uid={uid},gid={gid}", loop_dev, mount_point],
    ):
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            mounted = True
            break

    if not mounted:
        subprocess.run(["sudo", "losetup", "-d", loop_dev], capture_output=True)
        raise RuntimeError(
            f"mount failed even with sudo.\n"
            "Ensure exfat-fuse or exfatprogs is installed:\n"
            "  sudo apt install exfatprogs   # or exfat-fuse"
        )
    
    # ── 5. Copy files ──────────────────────────────────────────────────────────
    try:
        _linux_copy_files(source, mount_point, output_dir)
    except KeyboardInterrupt:
        print("\n\n⚠️  Copy interrupted by user (Ctrl+C). Cleaning up...")
        raise
    finally:
        print("🔒 Syncing and unmounting...")
        subprocess.run(["umount", mount_point],         capture_output=True)
        subprocess.run(["sudo", "umount", mount_point], capture_output=True)
        if loop_dev:
            subprocess.run(["losetup", "-d", loop_dev],         capture_output=True)
            subprocess.run(["sudo", "losetup", "-d", loop_dev], capture_output=True)
        shutil.rmtree(mount_point, ignore_errors=True)

# ==========================================
# MACOS IMPLEMENTATION
# ==========================================
def create_image_macos(source, output, size_mb, cluster_size, label):
    print(f"\n📦 Creating raw image ({human_readable_size(size_mb * 1024 * 1024)})...")
    run_cmd(["mkfile", "-n", f"{size_mb}m", str(output)])
    abs_output = str(output.resolve())
    print(f"🛠️ Formatting as exFAT (cluster={cluster_size}, label='{label}')...")
    run_cmd(["newfs_exfat", "-c", str(cluster_size), "-v", label, abs_output])

    print(f"📂 Mounting...")
    res = run_cmd(["hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", abs_output], capture=True)
    mount_point = None
    for line in res.stdout.splitlines():
        if "/Volumes/" in line:
            parts = line.split("\t")
            if len(parts) >= 3 and "/Volumes/" in parts[-1]:
                mount_point = parts[-1].strip()
                break
    if not mount_point:
        raise RuntimeError("Failed to find mount point from hdiutil output")
    
    try:
        dst_path = Path(mount_point)
        total_bytes = 0
        total_files = 0
        for dirpath, _, filenames in os.walk(source):
            for f in filenames:
                fp = Path(dirpath) / f
                if not fp.is_symlink():
                    total_bytes += fp.stat().st_size
                    total_files += 1

        print(f"📝 Copying {total_files} files ({human_readable_size(total_bytes)})...")
        with CopyProgressBar(total_bytes, total_files) as bar:
            parallel_copy_to_mount(source, dst_path, bar)
    finally:
        print(f"🔒 Syncing data to disk and unmounting (this may take a moment for large games)...")
        run_cmd(["hdiutil", "detach", mount_point])

# ==========================================
# WINDOWS IMPLEMENTATION
# ==========================================
def create_image_windows(source, output, size_mb, cluster_size, label):
    osfmount = shutil.which("osfmount.com")
    if not osfmount:
        for p in [r"C:\Program Files\OSFMount\osfmount.com", r"C:\Program Files (x86)\OSFMount\osfmount.com"]:
            if os.path.exists(p):
                osfmount = p
                break
    if not osfmount:
        raise RuntimeError("OSFMount is required on Windows to format raw images.")
        
    size_bytes = size_mb * 1024 * 1024
    print(f"\n📦 Creating raw image ({human_readable_size(size_bytes)})...")
    run_cmd(["fsutil", "file", "createnew", str(output), str(size_bytes)])

    used = set()
    for letter in "CDEFGHIJKLMNOPQRSTUVWXYZ":
        if os.path.exists(f"{letter}:\\"):
            used.add(letter)
    free_letters = [l for l in "FGHIJKLMNOPQRSTUVWXYZ" if l not in used]
    if not free_letters:
        raise RuntimeError("No free drive letters available!")
    drive_letter = free_letters[0]
    mount_point = f"{drive_letter}:"

    print(f"📂 Mounting raw image as {mount_point} via OSFMount...")
    run_cmd([osfmount, "-a", "-t", "file", "-f", str(output), "-m", mount_point, "-o", "rw"])

    try:
        print(f"🛠️ Formatting {mount_point} as exFAT (cluster={cluster_size}, label='{label}')...")
        # format.com is a .com console command CreateProcess cannot resolve when
        # invoked directly (it looks for format.exe -> WinError 2), and it prompts
        # even with /Y. Wrap in cmd.exe and pipe newlines to stdin -- this mirrors
        # the exFAT Image Builder app's proven Windows format path.
        # Windows `format` rejects raw byte values for /A: at this size
        # ("Invalid parameter - /A:65536") and requires K/M notation, so render
        # the cluster size as e.g. 64K / 32K (preserving the intended cluster).
        _alloc = (f"{cluster_size // (1024 * 1024)}M"
                  if cluster_size % (1024 * 1024) == 0
                  else f"{cluster_size // 1024}K")
        fmt_cmd = ["cmd.exe", "/c", "format", f"{mount_point}",
                   "/FS:exFAT", "/Q", "/Y", f"/A:{_alloc}", f"/V:{label}"]
        _fmt_flags = 0x08000000 if os.name == "nt" else 0  # CREATE_NO_WINDOW
        fmt_proc = subprocess.Popen(
            fmt_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace",
            creationflags=_fmt_flags)
        try:
            fmt_proc.stdin.write("\n\n\n")
            fmt_proc.stdin.flush()
            fmt_proc.stdin.close()
        except Exception:
            pass
        fmt_proc.wait(timeout=300)
        if fmt_proc.returncode != 0:
            raise RuntimeError(f"format /FS:exFAT failed (rc={fmt_proc.returncode})")
        try:
            fmt_proc.wait(timeout=300)
        except subprocess.TimeoutExpired:
            fmt_proc.kill()
            raise RuntimeError("format /FS:exFAT timed out after 5 minutes.")
        if fmt_proc.returncode != 0:
            raise RuntimeError(f"format /FS:exFAT failed (rc={fmt_proc.returncode})")

        # Copy destination MUST be the drive ROOT. Path("F:") is drive-RELATIVE
        # on Windows (yields "F:tdf\\..."); the root needs the trailing separator.
        # mount_point ("F:") stays as-is for the OSFMount -m argument.
        dst_path = Path(f"{drive_letter}:\\")
        total_bytes = 0
        total_files = 0
        for dirpath, _, filenames in os.walk(source):
            for f in filenames:
                fp = Path(dirpath) / f
                if not fp.is_symlink():
                    total_bytes += fp.stat().st_size
                    total_files += 1

        print(f"📝 Copying {total_files} files ({human_readable_size(total_bytes)})...")
        with CopyProgressBar(total_bytes, total_files) as bar:
            parallel_copy_to_mount(source, dst_path, bar)
    finally:
        print(f"🔒 Syncing data to disk and unmounting...")
        
        # Retry unmount up to 5 times to avoid "Access Denied" if Windows is slow to release locks
        unmounted = False
        for attempt in range(5):
            res = subprocess.run([osfmount, "-d", "-m", mount_point], capture_output=True, text=True)
            if res.returncode == 0:
                unmounted = True
                break
            time.sleep(1)
            
        if not unmounted:
            print("⚠️ Warning: OSFMount unmount failed (volume might be locked by another process).")

# ==========================================
# MAIN ENTRY POINT
# ==========================================
def create_exfat_image(
    source_folder: Path,
    output_path: Path,
    label: str = "PS5exfat",
    cluster_size: int | None = None,
    verbose: bool = True
) -> None:
    source = Path(source_folder).resolve()
    if not source.is_dir():
        raise FileNotFoundError(f"Source folder does not exist: {source}")
        
    output = Path(output_path).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    if output.suffix.lower() != ".exfat":
        output = output.with_suffix(".exfat")
        
    if output.exists():
        output.unlink()
        
    size_mb, chosen_cluster, total_raw, file_count = calculate_exfat_size(source, cluster_size)

    if verbose:
        print(f"🔍 Analyzing folder: {file_count} files, {human_readable_size(total_raw)}")
        print(f"📦 Target exFAT image size: {human_readable_size(size_mb * 1024 * 1024)} (cluster={chosen_cluster // 1024}K)")
        
    system = platform.system()
    try:
        if system == "Linux":
            create_image_linux(source, output, size_mb, chosen_cluster, label)
        elif system == "Darwin":
            create_image_macos(source, output, size_mb, chosen_cluster, label)
        elif system == "Windows":
            create_image_windows(source, output, size_mb, chosen_cluster, label)
        else:
            raise RuntimeError(f"Unsupported OS: {system}")
            
        if verbose:
            print(f"✅ Successfully created exFAT image: {output}")
    except Exception as e:
        if output.exists():
            output.unlink()
        raise RuntimeError(f"Failed to create exFAT image: {e}")