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
"""

from __future__ import annotations

import io
import re
import sys
import threading
import time
from typing import Callable

_RUN_LOCK = threading.Lock()

MAX_PROGRESS_HZ = 20          # max UI updates per second
_MIN_INTERVAL   = 1.0 / MAX_PROGRESS_HZ

_PROGRESS_RE = re.compile(
    r'\[([#\-]+)\]\s+(\d+)%\s+(\w+)'
    r'(?:\s+@\s+([\d.]+\s*\S+/s))?'
    r'(?:\s+ETA\s+(\S+))?'
)


def _mkpfs_available() -> bool:
    try:
        import importlib.util
        return importlib.util.find_spec('mkpfs') is not None
    except Exception:
        return False


def mkpfs_version() -> str | None:
    if not _mkpfs_available():
        return None
    try:
        import mkpfs
        return getattr(mkpfs, '__version__', None) or '(installed)'
    except Exception:
        return None


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
        # Always emit 0% (start) and 100% (done); throttle everything else
        if pct not in (0, 100) and (now - self._last_emit) < _MIN_INTERVAL:
            return
        self._last_emit = now
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


def run_mkpfs(argv: list[str],
              log_cb:      Callable[[str], None] | None = None,
              progress_cb: Callable[[str, int, int, str], None] | None = None
              ) -> int:
    """Run mkpfs in-process. Thread-safe via module-level lock.
    Returns exit code (0 = success).
    """
    if not _mkpfs_available():
        if log_cb:
            log_cb('mkpfs is not installed.')
        return 1

    from mkpfs.__main__ import main as mkpfs_main

    cap = _LineCapture(line_cb=log_cb, cr_cb=None, progress_cb=progress_cb)

    with _RUN_LOCK:
        old_stdout, old_stderr = sys.stdout, sys.stderr
        exit_code = 1
        try:
            sys.stdout = cap   # type: ignore[assignment]
            sys.stderr = cap   # type: ignore[assignment]
            result = mkpfs_main(argv)
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
