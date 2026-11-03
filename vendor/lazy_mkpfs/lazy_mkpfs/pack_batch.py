from __future__ import annotations
import sys
import os
import re
import time
import threading
import queue
import multiprocessing
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed

from .pack_folder import pack_folder
from .pack_file import pack_file

# Global queue reference for workers (injected via initializer)
BATCH_QUEUE = None

def worker_init(q):
    """Initializer to inject the progress queue into worker processes."""
    global BATCH_QUEUE
    BATCH_QUEUE = q

class WorkerOutputCapture:
    """Redirects worker stdout/stderr to parse progress and send to main process."""
    def __init__(self, q, game_name):
        self.queue = q
        self.game_name = game_name
        self.buffer = ""

    def write(self, text):
        if not isinstance(text, str):
            text = str(text)
        self.buffer += text
        
        # Process complete lines
        while '\r' in self.buffer or '\n' in self.buffer:
            parts = re.split(r'[\r\n]', self.buffer, maxsplit=1)
            line = parts[0]
            self.buffer = parts[1] if len(parts) > 1 else ""
            
            status = self._parse_status(line)
            if status:
                try:
                    self.queue.put_nowait((self.game_name, status))
                except Exception:
                    pass

    def _parse_status(self, line):
        # Extract percentage from progress bars like "[####----]  45%" or "│███░░░│ 45.0%"
        match = re.search(r'(\d+(?:\.\d+)?)\s*%', line)
        if match:
            return f"{match.group(1)}%"
        
        # Extract phase keywords
        if "Copying" in line: return "Copying..."
        if "Compressing" in line: return "Compressing..."
        if "Formatting" in line: return "Formatting..."
        if "Mounting" in line: return "Mounting..."
        if "Successfully" in line: return "✅ Done"
        if "Error" in line or "Failed" in line: return "❌ Error"
        return None

    def flush(self):
        pass

    def isatty(self):
        return False  # Prevents crashes from libraries checking if stdout is a terminal

class BatchDisplay:
    """Renders a clean, multi-line updating status for all active games."""
    def __init__(self):
        self.statuses = {}
        self.lock = threading.Lock()
        self.line_count = 0

    def update(self, game_name, status):
        with self.lock:
            self.statuses[game_name] = status
            self._render()

    def _render(self):
        # Move cursor up to overwrite the previous status block
        if self.line_count > 0:
            sys.stdout.write(f"\033[{self.line_count}A\033[J")
        
        self.line_count = len(self.statuses)
        for name, status in self.statuses.items():
            short_name = name[:45]
            sys.stdout.write(f"🎮 {short_name:<45} | {status}\n")
        sys.stdout.flush()

def _process_target_worker(args):
    """Worker function that processes a single target."""
    source_str, output_str, target_type, kwargs, game_name = args
    
    # Redirect all output from this worker to our progress parser
    sys.stdout = WorkerOutputCapture(BATCH_QUEUE, game_name)
    sys.stderr = WorkerOutputCapture(BATCH_QUEUE, game_name)
    
    source = Path(source_str)
    output = Path(output_str)
    
    try:
        if target_type == 'folder':
            stats = pack_folder(source_folder=source, output_image=output, **kwargs)
        else:
            stats = pack_file(source_file=source, output_image=output, **kwargs)
            
        return {
            'source': game_name, 'type': target_type, 'success': True, 'error': None,
            'gain': stats.actual_gain_pct, 'time': stats.elapsed_seconds
        }
    except Exception as e:
        return {
            'source': game_name, 'type': target_type, 'success': False, 'error': str(e),
            'gain': 0.0, 'time': 0.0
        }

def pack_batch(
    input_dir: str | Path,
    output_dir: str | Path,
    workers: int | None = None,
    zlib_backend: str = "zlib",
    zlib_level: int = 6,
    cpu_count: int = 0,
    use_ram_if_possible: bool = True,
    verbose: bool = False,
    exfat: bool = True,
) -> dict:
    in_dir = Path(input_dir).resolve()
    out_dir = Path(output_dir).resolve()

    if not in_dir.is_dir():
        raise FileNotFoundError(f"Input directory does not exist: {in_dir}")

    out_dir.mkdir(parents=True, exist_ok=True)
    
    targets = []
    supported_extensions = {'.exfat', '.ffpkg'}
    for item in in_dir.iterdir():
        if item.name.startswith('.'): continue
        if item.is_dir():
            targets.append({'type': 'folder', 'source': str(item), 'output': str(out_dir / f"{item.name}.ffpfsc")})
        elif item.is_file() and item.suffix.lower() in supported_extensions:
            targets.append({'type': 'file', 'source': str(item), 'output': str(out_dir / f"{item.stem}.ffpfsc")})
            
    targets = sorted(targets, key=lambda x: x['source'])

    if not targets:
        return {'total': 0, 'succeeded': 0, 'failed': 0, 'details': [], 'errors': []}

    total_cores = multiprocessing.cpu_count()
    num_workers = workers if workers is not None else max(1, min(total_cores - 1, 4))

    common_kwargs = {
        "zlib_backend": zlib_backend, "zlib_level": zlib_level, "cpu_count": cpu_count,
        "use_ram_if_possible": use_ram_if_possible, "verbose": verbose,
    }

    # Use Manager().Queue() to safely share across Windows processes
    manager = multiprocessing.Manager()
    q = manager.Queue()
    display = BatchDisplay()

    # Initialize display
    for t in targets:
        display.update(Path(t['source']).name, "⏳ Starting...")

    worker_args = []
    for t in targets:
        game_name = Path(t['source']).name
        kwargs = {**common_kwargs, "exfat": exfat} if t['type'] == 'folder' else common_kwargs.copy()
        worker_args.append((t['source'], t['output'], t['type'], kwargs, game_name))

    # Background thread to read progress from the queue and update the display
    stop_event = threading.Event()
    def queue_reader():
        while not stop_event.is_set() or not q.empty():
            try:
                msg = q.get(timeout=0.5)
                if msg is None: break
                game_name, status = msg
                display.update(game_name, status)
            except queue.Empty:
                pass

    reader_thread = threading.Thread(target=queue_reader, daemon=True)
    reader_thread.start()

    results = {'total': len(targets), 'succeeded': 0, 'failed': 0, 'details': [], 'errors': []}

    # Pass the queue to workers via the initializer
    with ProcessPoolExecutor(max_workers=num_workers, initializer=worker_init, initargs=(q,)) as executor:
        futures = {executor.submit(_process_target_worker, args): args[4] for args in worker_args}
        
        for future in as_completed(futures):
            res = future.result()
            results['details'].append(res)
            game_name = res['source']
            
            if res['success']:
                results['succeeded'] += 1
                display.update(game_name, f"✅ Done ({res['gain']:.1f}% gain)")
            else:
                results['failed'] += 1
                results['errors'].append(f"{game_name}: {res['error']}")
                display.update(game_name, f"❌ Failed")

    stop_event.set()
    q.put(None)
    reader_thread.join(timeout=2.0)
    print() # Final newline after the display block

    return results