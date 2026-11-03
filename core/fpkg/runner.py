"""Run the user's Publishing Tools and follow their output.

img_create prints a ruler line starting with '|____' and then up to 51
'=' characters, without newlines, as the image is written. The output is
read in small blocks so that progress can be shown while the bar grows;
the size of the file being written is polled as a second signal in case
the bar does not appear through a pipe.
"""
import os
import subprocess
import threading
import time

BAR_WIDTH = 51

ROUTINE_WARNINGS = (
    'The version of the SDK toolchain is invalid or missing',
    '(online check)',
    'has not been built for submission',
    '- Perform package verification',
    '- Rebuild the package for submission',
    'Process finished with warning(s)',
)

FRIENDLY_ERRORS = (
    ('reserved node found', 'A file or folder name is reserved by the '
     'package format; rename it in the dump.'),
    ('invalid attribute value dst_path', 'A file path is not accepted by '
     'the Publishing Tools (check for unusual characters).'),
    ('invalid attribute value src_path', 'A source path is not accepted by '
     'the Publishing Tools (non-ASCII folder names are a common cause).'),
    ('File not found', 'A file listed in the project was not found.'),
    ('ucp_digest is invalid', 'A trophy/UDS .ucp file in sce_sys has a '
     'stale digest.'),
    ('Format of the png file is not valid', 'A PNG in sce_sys has the wrong '
     'format (icon0/pic0/pic1 must be RGB, pic2 RGBA, 8-bit).'),
    ('NAPS metadata missing', 'The base package was not built by the '
     'Publishing Tools, so an update cannot be made against it.'),
    ('contentVersion must be higher', 'An update needs a higher '
     'contentVersion than its base package.'),
)


def classify(line):
    """'error' | 'warn' | 'info' | None (drop)."""
    s = line.strip()
    if not s or s.startswith('|____') or set(s) <= set('=|0123456789 %'):
        return None
    if s.startswith('[Error]'):
        return 'error'
    if s.startswith('[Warn]'):
        return 'info' if any(r in s for r in ROUTINE_WARNINGS) else 'warn'
    if s.startswith('wine:') or ':err:' in s:
        return 'warn'
    return 'info'


def explain(error_lines):
    """Friendly explanations for known SDK errors (deduplicated)."""
    out = []
    for line in error_lines:
        for needle, text in FRIENDLY_ERRORS:
            if needle in line and text not in out:
                out.append(text)
    return out


class RunResult:
    def __init__(self):
        self.returncode = None
        self.cancelled = False
        self.errors = []
        self.warnings = []
        self.lines = []
        self.elapsed = 0.0

    @property
    def ok(self):
        return self.returncode == 0 and not self.errors and not self.cancelled


class _BarParser:
    """Feeds raw output text; yields complete lines and tracks the bar."""

    def __init__(self, on_line, on_bar):
        self._buf = ''
        self._in_bar = False
        self._bars = 0
        self._on_line = on_line
        self._on_bar = on_bar

    def feed(self, text):
        for ch in text:
            if self._in_bar and ch == '=':
                self._bars += 1
                self._on_bar(min(self._bars, BAR_WIDTH) / BAR_WIDTH)
                if self._bars >= BAR_WIDTH:
                    self._in_bar = False
                continue
            if ch in '\r\n':
                line, self._buf = self._buf, ''
                if line.startswith('|____'):
                    self._in_bar = True
                    self._bars = 0
                elif line.strip():
                    if self._in_bar and any(c.isalpha() for c in line):
                        self._in_bar = False
                    self._on_line(line)
                continue
            self._buf += ch

    def close(self):
        if self._buf.strip():
            self._on_line(self._buf)
        self._buf = ''


def kill_tree(proc):
    if proc.poll() is not None:
        return
    if os.name == 'nt':
        try:
            subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'],
                           capture_output=True, timeout=30,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except Exception:
            pass
    try:
        proc.kill()
    except Exception:
        pass


def run(argv, cwd=None, env=None, log=None, progress=None, cancel_check=None,
        watch_file=None, poll=0.5):
    """Run a Publishing Tools command.

    log(kind, line), progress(fraction or None, bytes_written or None),
    cancel_check() -> True to stop. Returns a RunResult."""
    res = RunResult()
    log = log or (lambda k, s: None)
    progress = progress or (lambda f, b: None)
    t0 = time.time()
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0) | \
        getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)

    def on_line(line):
        res.lines.append(line)
        kind = classify(line)
        if kind is None:
            return
        if kind == 'error':
            res.errors.append(line.strip())
        elif kind == 'warn':
            res.warnings.append(line.strip())
        log(kind, line.rstrip())

    lock = threading.Lock()

    def on_line_locked(line):
        with lock:
            on_line(line)

    proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            creationflags=flags)
    enc = 'mbcs' if os.name == 'nt' else 'utf-8'

    def pump(stream, with_bar):
        parser = _BarParser(on_line_locked,
                            (lambda f: progress(f, None)) if with_bar
                            else (lambda f: None))
        fd = stream.fileno()
        while True:
            try:
                chunk = os.read(fd, 256)
            except OSError:
                break
            if not chunk:
                break
            parser.feed(chunk.decode(enc, errors='replace'))
        parser.close()

    readers = [threading.Thread(target=pump, args=(proc.stdout, True), daemon=True),
               threading.Thread(target=pump, args=(proc.stderr, False), daemon=True)]
    for r in readers:
        r.start()
    last_size = -1
    while proc.poll() is None:
        if cancel_check and cancel_check():
            res.cancelled = True
            kill_tree(proc)
            break
        if watch_file:
            try:
                sz = os.path.getsize(watch_file)
                if sz != last_size:
                    last_size = sz
                    progress(None, sz)
            except OSError:
                pass
        time.sleep(poll)
    proc.wait()
    for r in readers:
        r.join(timeout=10)
    for stream in (proc.stdout, proc.stderr):
        try:
            stream.close()
        except Exception:
            pass
    res.returncode = proc.returncode
    res.elapsed = time.time() - t0
    return res
