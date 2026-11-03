"""ui/mkpfs_runner.py — in-process mkpfs execution for the frozen exe.

Progress capture fix
--------------------
mkpfs's Progress class writes:   sys.stderr.write(f"\\r{line}{padding}")

The \\r is at position 0 of every write() call. The content is what
FOLLOWS it. The correct mental model is a terminal "carriage return":
go to the start of the line and overwrite with what comes next.

So the rule is:
  - When we see \\r, everything currently in the buffer (before the \\r)
    is the "previous line content" — emit it via cr_cb if non-empty.
  - Then the content after the \\r becomes the new buffer start.
  - When we see \\n, everything in the buffer up to it is a complete log
    line — emit via line_cb.

Updates are throttled to MAX_PROGRESS_HZ per second to avoid flooding
the Tk event queue with parent.after() calls on fast storage.

Public API
----------
    run_mkpfs(argv, log_cb=None, progress_cb=None) -> int
    mkpfs_version() -> str | None
    mkpfs_version_tuple() -> tuple[int, ...] | None
    mkpfs_supports_deep_unpack() -> bool
    VerifyResult (enum: PASS / FAIL / TOOL_ERROR)
    classify_verify_result(rc, stdout='', stderr='') -> VerifyResult
"""

from __future__ import annotations

import enum
import io
import os
import re
import subprocess
import sys
import threading
import time
from typing import Callable

_RUN_LOCK = threading.Lock()

MAX_PROGRESS_HZ = 4           # max UI updates per second
_MIN_INTERVAL   = 1.0 / MAX_PROGRESS_HZ

_PROGRESS_RE = re.compile(
    r'\[([#\-]+)\]\s+(\d+)%\s+(\w+)'
    r'(?:\s+@\s+([\d.]+\s*\S+/s))?'
    r'(?:\s+ETA\s+(\S+))?'
)


# ---------------------------------------------------------------------------
# Verify-result classification (single source of truth for the app)
# ---------------------------------------------------------------------------
class VerifyResult(enum.Enum):
    """Outcome of an `mkpfs verify` run, decoupled from raw rc/text."""
    PASS = enum.auto()
    FAIL = enum.auto()
    TOOL_ERROR = enum.auto()


# Substrings (lower-cased) that mean "MkPFS could not run the command at all"
# rather than "the image failed verification". Matched against stdout+stderr.
_EXEC_ERROR_SIGNATURES = (
    'usage:',                  # argparse rejected the invocation
    'unrecognized arguments',
    'invalid choice',
    'no such file',            # missing input / tool resource
    'errno 2',
    'permission denied',
    'errno 13',
)

# Return codes argparse / the CLI use for "bad invocation" (not a verdict).
_EXEC_ERROR_RETURN_CODES = (2,)


def classify_verify_result(rc: int,
                           stdout: str = '',
                           stderr: str = '') -> 'VerifyResult':
    """Classify an `mkpfs verify` run into PASS / FAIL / TOOL_ERROR.

    Args:
        rc: the process exit code from run_mkpfs.
        stdout / stderr: MkPFS's captured output. Either may be empty; pass
            whatever was captured (the app folds both into one stream, so
            passing the combined text as `stdout` is fine).

    Rules (single source of truth):
        * rc == 0                       -> PASS.
        * rc in _EXEC_ERROR_RETURN_CODES,
          or output matches an _EXEC_ERROR_SIGNATURES entry
                                        -> TOOL_ERROR (caller falls back).
        * otherwise (rc != 0, no exec-error signature)
                                        -> FAIL (genuine negative verdict).

    Note: the caller is expected to confirm the input image exists BEFORE
    running verify, so a 'no such file' here unambiguously means a tool error
    (MkPFS could not open something), never an applicability question.
    """
    if rc == 0:
        return VerifyResult.PASS
    blob = ((stdout or '') + '\n' + (stderr or '')).lower()
    if rc in _EXEC_ERROR_RETURN_CODES:
        return VerifyResult.TOOL_ERROR
    for sig in _EXEC_ERROR_SIGNATURES:
        if sig in blob:
            return VerifyResult.TOOL_ERROR
    return VerifyResult.FAIL


def _mkpfs_available() -> bool:
    try:
        import importlib.util
        return importlib.util.find_spec('mkpfs') is not None
    except Exception:
        return False


def mkpfs_version() -> str | None:
    """Return the bundled MkPFS version string (e.g. '1.0.0'), '(installed)'
    if the package is importable but exposes no __version__, or None if MkPFS
    is not available at all.

    Resolution order:
      1. `mkpfs.__version__` (PSBrew MkPFS exposes this since the 1.x line).
      2. importlib.metadata for the distribution named 'mkpfs' (covers the
         pip-installed / frozen-collected case where the attribute path is
         shadowed).
      3. '(installed)' as a last-resort truthy sentinel so callers that only
         check truthiness (availability) keep working.
    """
    if not _mkpfs_available():
        return None
    # 1. package attribute
    try:
        import mkpfs
        ver = getattr(mkpfs, '__version__', None)
        if ver:
            return str(ver)
    except Exception:
        pass
    # 2. distribution metadata
    try:
        from importlib import metadata as _md
        ver = _md.version('mkpfs')
        if ver:
            return str(ver)
    except Exception:
        pass
    return '(installed)'


def mkpfs_version_tuple() -> tuple[int, ...] | None:
    """Best-effort numeric parse of mkpfs_version() for comparisons, e.g.
    (1, 0, 0). Returns None if the version is unknown/unparseable. Used for
    feature-gating (the OSFMount-free `unpack --deep` path lands in Stage 2 and
    needs MkPFS >= 1.0.0)."""
    v = mkpfs_version()
    if not v or v == '(installed)':
        return None
    try:
        nums = re.findall(r'\d+', v)
        if not nums:
            return None
        return tuple(int(n) for n in nums[:3])
    except Exception:
        return None


def mkpfs_supports_deep_unpack() -> bool:
    """Whether the bundled MkPFS can do OSFMount-free deep extraction
    (`unpack --deep`) and native exFAT reads.

    This is a *capability* probe, not a version-number gate. The native exFAT
    reader (`mkpfs.exfat`) and the `unpack --deep` flag landed in the 0.0.9
    release and are present in every build at/after it (including the GitHub
    1.0.0 line). We detect the capability directly so the OSFMount-free path
    works on any MkPFS that actually has it, regardless of how the version
    string reads.

    Detection (all must hold):
      1. mkpfs is importable.
      2. The native exFAT reader module `mkpfs.exfat` exists (this is what makes
         mount-free reads possible).
      3. The `mkpfs.exfat` reader exposes its public entry (`open_exfat`).

    NOTE (Stage 0/1): informational only right now — drives the version/
    capability banner. Extract and verify still use the existing OSFMount path.
    Stage 2 consumes this to route around OSFMount when MkPFS is selected.
    """
    if not _mkpfs_available():
        return False
    try:
        import importlib.util
        if importlib.util.find_spec('mkpfs.exfat') is None:
            return False
    except Exception:
        return False
    # The reader module exists; confirm its public entry point is present.
    try:
        import mkpfs.exfat as _mx
        return hasattr(_mx, 'open_exfat')
    except Exception:
        return False


def _parse_progress(line: str):
    """Parse a mkpfs Progress bar line.
    Returns (phase, pct, total, detail) or None.
    """
    m = _PROGRESS_RE.search(line)
    if not m:
        return None
    phase  = m.group(3)
    pct    = int(m.group(2))
    speed  = m.group(4) or ''
    eta    = m.group(5) or ''
    detail = phase
    if speed:
        detail += '  @  ' + speed
    if eta:
        detail += '  ETA ' + eta
    return (phase, pct, 100, detail)


class _LineCapture(io.TextIOBase):
    """Captures a text stream and fires callbacks for log lines and
    progress-bar overwrites.

    \\r = terminal carriage return: content AFTER \\r is new line display.
         Emit the content currently in the buffer (before \\r) via cr_cb.
    \\n = newline: emit buffer up to \\n via line_cb.
    """

    def __init__(self,
                 line_cb:  Callable[[str], None] | None = None,
                 cr_cb:    Callable[[str], None] | None = None,
                 progress_cb: Callable | None = None):
        super().__init__()
        self._line_cb     = line_cb
        self._cr_cb       = cr_cb
        self._progress_cb = progress_cb
        self._buf         = ''
        self._last_emit   = 0.0    # monotonic time of last progress_cb call
        self._last_pct    = -1     # last pct delivered (dedupe identical ticks)

    def write(self, s: str) -> int:
        self._buf += s
        while True:
            cr = self._buf.find('\r')
            nl = self._buf.find('\n')

            if cr == -1 and nl == -1:
                break

            if nl != -1 and (cr == -1 or nl < cr):
                # \n first — emit buf[:nl] as a log line
                line = self._buf[:nl]
                self._buf = self._buf[nl + 1:]
                if line.strip():
                    self._emit_log(line)
                    # A \n-terminated line that looks like progress is the
                    # "phase complete" update — fire progress_cb too.
                    self._maybe_progress(line)
            else:
                # \r first — content BEFORE \r is the current line; emit it
                current = self._buf[:cr]
                self._buf = self._buf[cr + 1:]
                if current.strip():
                    if self._cr_cb:
                        try:
                            self._cr_cb(current)
                        except Exception:
                            pass
                    self._maybe_progress(current)

        return len(s)

    def _emit_log(self, line: str) -> None:
        if self._line_cb:
            try:
                self._line_cb(line)
            except Exception:
                pass

    def _maybe_progress(self, line: str) -> None:
        """Parse and throttle progress callbacks."""
        if not self._progress_cb:
            return
        parsed = _parse_progress(line)
        if not parsed:
            return
        now = time.monotonic()
        phase, pct, total, detail = parsed
        # Always emit 0% (start) and 100% (done); throttle everything else.
        # Also drop ticks whose pct did not change (mkpfs redraws the bar
        # many times per percent on fast storage).
        if pct not in (0, 100):
            if (now - self._last_emit) < _MIN_INTERVAL:
                return
            if pct == self._last_pct:
                return
        self._last_emit = now
        self._last_pct = pct
        try:
            self._progress_cb(phase, pct, total, detail)
        except Exception:
            pass

    def flush(self) -> None:
        # Remaining buffer content is an in-progress \r line (no terminator
        # yet). Treat it as a progress update, not a log line — routing it
        # through line_cb would flood the log with every single progress tick.
        if self._buf.strip():
            if self._cr_cb:
                try:
                    self._cr_cb(self._buf)
                except Exception:
                    pass
            self._maybe_progress(self._buf)
        self._buf = ''


def _is_frozen() -> bool:
    return bool(getattr(sys, 'frozen', False))


def _mkpfs_worker_cmd(argv: list[str]) -> list[str] | None:
    """Build the command line to run mkpfs in a SEPARATE process.

    Frozen build: re-invoke this exe in mkpfs-worker mode (sys.executable
    is the exe; the __main__ shim handles '--__mkpfs_worker__'). Dev:
    'python -m mkpfs'. Returns None if no runnable form is available.
    """
    if _is_frozen():
        return [sys.executable, '--__mkpfs_worker__'] + list(argv)
    exe = sys.executable
    if exe:
        return [exe, '-m', 'mkpfs'] + list(argv)
    return None


def _run_mkpfs_subprocess(argv: list[str], cap: '_LineCapture',
                          log_cb, cancel_check=None) -> int | None:
    """Option B: run mkpfs as a child process and pump its stdout+stderr
    through `cap` (same parsing as in-process). Returns the exit code, or
    None if the process could not be started (so the caller can fall back).
    """
    cmd = _mkpfs_worker_cmd(argv)
    if not cmd:
        return None

    # No console window for the child on Windows.
    creationflags = 0
    startupinfo = None
    if os.name == 'nt':
        creationflags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        try:
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        except Exception:
            startupinfo = None

    # mkpfs's Pool workers re-exec this same worker command; make sure the
    # child knows it is a frozen-mp child (PyInstaller sets this up, but be
    # explicit and harmless on dev).
    env = dict(os.environ)
    # Force UTF-8 stdio in the (frozen) child so mkpfs can print Unicode
    # source paths (e.g. a folder name containing 'ō') without a CP1252
    # UnicodeEncodeError('charmap'). Propagates to mkpfs's mp Pool children,
    # which re-exec this same worker and inherit the env. The parent decodes
    # the child's stream as UTF-8 with errors='replace' (see the read loop
    # below), so the two sides match. ASCII output is unaffected (UTF-8 is an
    # ASCII superset).
    env['PYTHONIOENCODING'] = 'utf-8:replace'

    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,   # fold stderr (progress) into one stream
            stdin=subprocess.DEVNULL,
            bufsize=0,                  # unbuffered: \r progress arrives live
            env=env,
            creationflags=creationflags,
            startupinfo=startupinfo,
        )
    except Exception as e:
        if log_cb:
            log_cb('mkpfs subprocess could not start (%s); '
                   'falling back to in-process.' % e)
        return None

    # Stream raw bytes so carriage-return progress updates are not line-
    # buffered away. Decode loosely and feed the capture char-chunk by chunk.
    #
    # rc5 audit fix H1: the read happens on a helper thread that pumps chunks
    # into a queue, so this loop can poll `cancel_check` between reads
    # instead of blocking in read(). On cancel the child is terminated and
    # we fall through to the normal wait()/return path.
    import threading as _thr
    import queue as _q
    _outq = _q.Queue()

    def _reader():
        try:
            assert proc.stdout is not None
            while True:
                _c = proc.stdout.read(4096)
                if not _c:
                    break
                _outq.put(_c)
        except Exception as _re:
            if log_cb:
                try:
                    log_cb('mkpfs subprocess read error: %s' % _re)
                except Exception:
                    pass
        finally:
            _outq.put(None)

    _thr.Thread(target=_reader, daemon=True).start()
    _cancelled = False
    try:
        _eof = False
        while not _eof:
            if not _cancelled and cancel_check is not None:
                try:
                    if cancel_check():
                        _cancelled = True
                        if log_cb:
                            log_cb('Cancel requested \u2014 terminating mkpfs worker.')
                        # terminate, then wait briefly; kill if it lingers
                        try:
                            proc.terminate()
                        except Exception:
                            pass
                        try:
                            proc.wait(timeout=5)
                        except Exception:
                            try:
                                proc.kill()
                            except Exception:
                                pass
                except Exception:
                    pass
            try:
                chunk = _outq.get(timeout=0.25)
            except _q.Empty:
                if proc.poll() is not None and _outq.empty():
                    break
                continue
            if chunk is None:
                _eof = True
                continue
            try:
                cap.write(chunk.decode('utf-8', errors='replace'))
            except Exception:
                pass
    finally:
        try:
            proc.stdout.close()  # type: ignore[union-attr]
        except Exception:
            pass
        rc = proc.wait()
    return rc


def run_mkpfs(argv: list[str],
              log_cb:      Callable[[str], None] | None = None,
              progress_cb: Callable[[str, int, int, str], None] | None = None,
              cancel_check: Callable[[], bool] | None = None
              ) -> int:
    """Run mkpfs and return its exit code (0 = success).

    v3.6.2 (Option B): mkpfs runs in a SEPARATE process so its compression
    Pool can never re-enter this frozen GUI module (the cause of the
    'NoneType has no attribute write' / BrokenPipeError 232 crash). If the
    subprocess cannot be started, we fall back to running mkpfs in-process
    forced to a SINGLE core (--cpu-count 1), which avoids the Pool path
    entirely. Thread-safe via the module-level lock.

    rc5 audit fix H1: `cancel_check` (optional, callable -> bool) is polled
    while the subprocess runs; on cancel the child is terminated and this
    returns its (non-zero) exit code. The caller decides cancel semantics
    by consulting its own cancel flag after return — no sentinel rc is
    invented here. The in-process fallback cannot be interrupted, so a
    pending cancel skips it entirely.
    """
    if not _mkpfs_available() and not _is_frozen():
        if log_cb:
            log_cb('mkpfs is not installed.')
        return 1

    cap = _LineCapture(line_cb=log_cb, cr_cb=None, progress_cb=progress_cb)

    with _RUN_LOCK:
        # --- Primary path: separate process ---
        rc = _run_mkpfs_subprocess(argv, cap, log_cb,
                                   cancel_check=cancel_check)
        if rc is not None:
            cap.flush()
            return rc

        # A pending cancel means the user does not want the in-process
        # fallback (it cannot be interrupted) — honour it and bail out.
        if cancel_check is not None:
            try:
                if cancel_check():
                    if log_cb:
                        log_cb('Cancel requested \u2014 skipping in-process fallback.')
                    cap.flush()
                    return 1
            except Exception:
                pass

        # --- Fallback: in-process, single core (no Pool) ---
        if log_cb:
            log_cb('Running mkpfs in-process (single-core fallback).')
        fb_argv = list(argv)
        try:
            _ci = fb_argv.index('--cpu-count')
            fb_argv[_ci + 1] = '1'
        except (ValueError, IndexError):
            fb_argv += ['--cpu-count', '1']

        try:
            from mkpfs.__main__ import main as mkpfs_main
        except Exception as e:
            if log_cb:
                log_cb('mkpfs import failed: %s' % e)
            cap.flush()
            return 1

        old_stdout, old_stderr = sys.stdout, sys.stderr
        exit_code = 1
        try:
            sys.stdout = cap   # type: ignore[assignment]
            sys.stderr = cap   # type: ignore[assignment]
            result = mkpfs_main(fb_argv)
            exit_code = result if isinstance(result, int) else 0
        except SystemExit as e:
            exit_code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
        except Exception as e:
            if log_cb:
                log_cb('mkpfs error: ' + str(e))
            exit_code = 1
        finally:
            sys.stdout = old_stdout
            sys.stderr = old_stderr
            cap.flush()

    return exit_code
