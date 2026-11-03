"""ui/progress_state.py — unified build progress model (v4.0.0).

A single, backend-agnostic progress model that every build backend can
populate and that the UI reads from, instead of each stage parsing backend
output in its own way. Introduced to fix the "frozen at ~82%" symptom on
exFAT -> .ffpfsc builds, where the Lazy_MkPFS compression stage produced no
progress-bar updates because nothing parsed its stdout.

Design rules (do not violate):
  * The PRIMARY truth source is BYTES processed / total bytes. That drives
    the main percentage whenever a trustworthy byte total exists.
  * NEVER fabricate progress. No elapsed-time-based percentages, no hardcoded
    95% / 98% / 99% transitions, no jumping forward without evidence. If no
    trustworthy metric is available, the model goes INDETERMINATE and the UI
    shows a spinner-style "backend running" message instead of a fake number.
  * Percentage priority, highest first:
        1. real byte progress (bytes_done / bytes_total)
        2. a percentage the backend emitted directly
        3. (caller may supply copy-stage byte progress the same way as #1)
        4. indeterminate
  * Updates are monotonic for the displayed percent: a parsed value that would
    move the bar BACKWARDS is ignored (kept at the last known max) unless the
    caller explicitly resets for a new stage. This prevents a noisy backend
    line from yanking the bar down.

This module is intentionally Tkinter-free and dependency-free so it can be
unit-tested without a GUI.
"""

from __future__ import annotations

import re
import time


# ---------------------------------------------------------------------------
# Unit helpers
# ---------------------------------------------------------------------------
_UNIT_MULT = {
    "b": 1,
    "kb": 1024,
    "kib": 1024,
    "k": 1024,
    "mb": 1024 ** 2,
    "mib": 1024 ** 2,
    "m": 1024 ** 2,
    "gb": 1024 ** 3,
    "gib": 1024 ** 3,
    "g": 1024 ** 3,
    "tb": 1024 ** 4,
    "tib": 1024 ** 4,
    "t": 1024 ** 4,
}


def _to_bytes(value: float, unit: str) -> float:
    return float(value) * _UNIT_MULT.get((unit or "b").strip().lower(), 1)


def fmt_bytes(n) -> str:
    """Human-readable size, GB-biased for PS5-scale numbers."""
    try:
        n = float(n)
    except Exception:
        return "?"
    if n < 0:
        return "?"
    for unit, div in (("TB", 1024 ** 4), ("GB", 1024 ** 3),
                      ("MB", 1024 ** 2), ("KB", 1024)):
        if n >= div:
            return "%.2f %s" % (n / div, unit)
    return "%d B" % int(n)


def fmt_speed(bps) -> str:
    try:
        bps = float(bps)
    except Exception:
        return ""
    if bps <= 0:
        return ""
    for unit, div in (("GB/s", 1024 ** 3), ("MB/s", 1024 ** 2),
                      ("KB/s", 1024)):
        if bps >= div:
            return "%.1f %s" % (bps / div, unit)
    return "%d B/s" % int(bps)


def fmt_eta(seconds) -> str:
    try:
        s = int(round(float(seconds)))
    except Exception:
        return ""
    if s < 0:
        return ""
    if s >= 3600:
        h, rem = divmod(s, 3600)
        m, sec = divmod(rem, 60)
        return "%dh %dm" % (h, m)
    if s >= 60:
        m, sec = divmod(s, 60)
        return "%dm %02ds" % (m, sec)
    return "%ds" % s


def fmt_count(n) -> str:
    try:
        return "{:,}".format(int(n))
    except Exception:
        return str(n)


# ---------------------------------------------------------------------------
# Backend-output parsing
# ---------------------------------------------------------------------------
# "12.3 GB / 45.6 GB", "1234567/9876543 bytes", "12.3GB of 45.6GB"
_RE_BYTES_PAIR = re.compile(
    r"(?P<dn>[\d.,]+)\s*(?P<du>[KMGT]i?B|B|[KMGT])?\s*/\s*"
    r"(?P<tot>[\d.,]+)\s*(?P<tu>[KMGT]i?B|B|[KMGT])?\b",
    re.IGNORECASE)
# "files 123/456", "123 of 456 files", "file 123 / 456"
_RE_FILES = re.compile(
    r"(?:files?\s*[:=]?\s*)?(?P<fd>[\d,]+)\s*(?:/|of)\s*(?P<ft>[\d,]+)\s*files?\b",
    re.IGNORECASE)
_RE_FILES_LABELLED = re.compile(
    r"files?\s*[:=]?\s*(?P<fd>[\d,]+)\s*(?:/|of)\s*(?P<ft>[\d,]+)",
    re.IGNORECASE)
# "42%", "42.5 %"  (but not "100.0.1%" or inside a number like 1.42%)
# Captures 1-3 digits with optional decimals.
_RE_PCT = re.compile(r"(?<![\d.])(\d{1,3}(?:\.\d+)?)\s*%")
# "ETA 00:12:34", "ETA 12:34", "remaining 1:02:03", "time left: 12:34"
_RE_ETA_HMS = re.compile(
    r"(?:ETA|remaining|time\s*left)\s*[:=]?\s*"
    r"(?:(\d+):)?(\d{1,2}):(\d{2})",
    re.IGNORECASE)
_RE_ETA_UNITS = re.compile(
    r"(?:ETA|remaining|time\s*left)\s*[:=]?\s*"
    r"(?:(\d+)\s*h)?\s*(?:(\d+)\s*m)?\s*(?:(\d+)\s*s)?",
    re.IGNORECASE)
# "123.4 MB/s", "1.2 GB/s", "@ 45 MB/s", "speed: 12 MB/s"
_RE_SPEED = re.compile(
    r"(?:@|speed\s*[:=]?)?\s*(?P<sv>[\d.,]+)\s*(?P<su>[KMGT]i?B|B)\s*/\s*s\b",
    re.IGNORECASE)
# A plausible file token: something with a letter, a dot and a short
# extension, not preceded by a word char / dot (so "1.5" and "v4.0.2" don't
# count). Paths with / or \ are reduced to their basename by the caller.
# Backslash and slash are excluded from the token itself so a Windows path
# yields its last component.
_RE_FILE_TOKEN = re.compile(
    r"(?<![\w.])(?P<f>[^\s\r\n/\\]*[A-Za-z_][^\s\r\n/\\]*\.[A-Za-z][A-Za-z0-9_]{0,7})(?![\w])")
# Extensions we trust as "a file being processed" even without a context
# verb nearby. Anything else needs one of the _FILE_CONTEXT words on the
# same line to count (keeps "exfat_builder.exe" / "v4.0.2.txt" style noise
# from being shown as the current file).
_FILE_EXT_ALLOW = (
    ".ucas", ".ucm", ".uasset", ".umap", ".pak", ".bin", ".dat", ".img",
    ".pkg", ".ffpkg", ".exfat", ".ffpfsc", ".sig", ".prx", ".sprx", ".elf",
    ".self", ".so", ".bnk", ".at9", ".usm",
)
# Context words that make a file token trustworthy on their own line.
_FILE_CONTEXT = re.compile(
    r"\b(current|compress|pack|writ|adding|process|deflat|file)\b",
    re.IGNORECASE)
# Bare phase words -> stage label (first match wins; order matters).
_PHASE_WORDS = (
    ("finaliz", "Finalizing"),
    ("compress", "Compressing"),
    ("pack", "Packing"),
    ("writ", "Writing"),
    ("deflat", "Compressing"),
    ("verif", "Verifying"),
    ("done", "Done"),
    ("complet", "Done"),
)


def _num(s: str) -> float:
    return float(str(s).replace(",", ""))


class ProgressState:
    """Mutable, backend-agnostic build progress.

    Fields map 1:1 to the model the UI renders. `percent` is computed from
    bytes when possible, else from a backend-emitted percentage, else left at
    its last value (or None when indeterminate). `indeterminate` is True when
    we have no trustworthy metric and the UI should show a spinner-style
    "still running" state rather than a number.
    """

    __slots__ = ("stage", "bytes_done", "bytes_total", "files_done",
                 "files_total", "current_file", "speed_bps", "eta_seconds",
                 "_percent", "indeterminate", "last_update", "_max_percent",
                 "_last_bytes", "_last_bytes_t")

    def __init__(self, stage: str = ""):
        self.stage = stage
        self.bytes_done = None
        self.bytes_total = None
        self.files_done = None
        self.files_total = None
        self.current_file = ""
        self.speed_bps = None
        self.eta_seconds = None
        self._percent = None
        self.indeterminate = False
        self.last_update = time.monotonic()
        self._max_percent = 0.0
        self._last_bytes = None
        self._last_bytes_t = None

    # -- derived -----------------------------------------------------------
    @property
    def percent(self):
        # 1. bytes
        if self.bytes_total and self.bytes_total > 0 \
                and self.bytes_done is not None and self.bytes_done >= 0:
            p = 100.0 * float(self.bytes_done) / float(self.bytes_total)
            p = max(0.0, min(100.0, p))
            self._max_percent = max(self._max_percent, p)
            return self._max_percent
        # 2. backend-emitted percentage
        if self._percent is not None:
            self._max_percent = max(self._max_percent, self._percent)
            return self._max_percent
        # 3. nothing trustworthy
        return None

    def reset_for_stage(self, stage: str):
        """Begin a new stage. Clears the monotonic ceiling so the bar can
        legitimately restart for, e.g., a verify pass after compression."""
        self.stage = stage
        self.bytes_done = None
        self.bytes_total = None
        self.files_done = None
        self.files_total = None
        self.current_file = ""
        self.speed_bps = None
        self.eta_seconds = None
        self._percent = None
        self.indeterminate = False
        self._max_percent = 0.0
        self._last_bytes = None
        self._last_bytes_t = None
        self.last_update = time.monotonic()

    # -- display -----------------------------------------------------------
    def main_line(self, verb: str = "Building") -> str:
        """Primary line, e.g. 'Building 92.4 GB / 285.1 GB (32%)'."""
        if self.bytes_total and self.bytes_total > 0 \
                and self.bytes_done is not None:
            pct = self.percent
            pct_s = (" (%d%%)" % int(pct)) if pct is not None else ""
            return f"{verb!s} {fmt_bytes(self.bytes_done)!s} / " \
                   f"{fmt_bytes(self.bytes_total)!s}{pct_s!s}"
        if self._percent is not None:
            return "%s %d%%" % (verb, int(self.percent or 0))
        return "Backend running — waiting for progress information."

    def secondary_line(self) -> str:
        """e.g. 'Files 18,422 / 56,103 • 1.2 GB/s • ETA 2m 41s'."""
        parts = []
        if self.files_total and self.files_total > 0 \
                and self.files_done is not None:
            parts.append(f"Files {fmt_count(self.files_done)!s} / "
                         f"{fmt_count(self.files_total)!s}")
        sp = fmt_speed(self.speed_bps)
        if sp:
            parts.append(sp)
        eta = fmt_eta(self.eta_seconds)
        if eta:
            parts.append("ETA " + eta)
        return "  •  ".join(parts)


def parse_backend_line(line: str, state: ProgressState,
                       derive_speed: bool = True) -> bool:
    """Opportunistically update `state` from one backend output fragment.

    Format-agnostic: detects byte pairs, file pairs, a percentage, ETA,
    speed, current file, and bare phase words, in whatever combination the
    line happens to contain. Returns True if ANY trustworthy signal was
    found (so the caller can refresh the UI and pet the watchdog), False if
    the line carried nothing usable.

    Never fabricates: if a line has no recognizable metric this returns False
    and leaves `state` untouched (except it does NOT bump last_update).
    """
    if not line:
        return False
    found = False
    low = line.lower()

    # -- bytes pair ---------------------------------------------------------
    # Require at least one explicit unit so "3/4" alone isn't mistaken.
    m = _RE_BYTES_PAIR.search(line)
    if m and (m.group("du") or m.group("tu") or "byte" in low):
        try:
            dn = _to_bytes(_num(m.group("dn")), m.group("du") or "b")
            tot = _to_bytes(_num(m.group("tot")), m.group("tu") or
                            m.group("du") or "b")
            if tot > 0 and 0 <= dn <= tot * 4:
                # derive speed/ETA from consecutive byte samples
                if derive_speed and state.bytes_done is not None \
                        and state._last_bytes_t is not None:
                    dt = time.monotonic() - state._last_bytes_t
                    db = dn - (state._last_bytes if state._last_bytes
                               is not None else dn)
                    if dt > 0.05 and db > 0:
                        state.speed_bps = db / dt
                        if state.speed_bps > 0:
                            state.eta_seconds = max(0.0, (tot - dn) /
                                                    state.speed_bps)
                state._last_bytes = dn
                state._last_bytes_t = time.monotonic()
                state.bytes_done = dn
                state.bytes_total = tot
                found = True
        except Exception:
            pass

    # -- files pair ---------------------------------------------------------
    m = _RE_FILES_LABELLED.search(line) or _RE_FILES.search(line)
    if m:
        try:
            fd = int(_num(m.group("fd")))
            ft = int(_num(m.group("ft")))
            if ft > 0 and 0 <= fd <= ft * 4:
                state.files_done = fd
                state.files_total = ft
                found = True
        except Exception:
            pass

    # -- explicit speed -----------------------------------------------------
    m = _RE_SPEED.search(line)
    if m:
        try:
            state.speed_bps = _to_bytes(_num(m.group("sv")), m.group("su"))
            found = True
        except Exception:
            pass

    # -- ETA ----------------------------------------------------------------
    m = _RE_ETA_HMS.search(line)
    if m:
        try:
            h = int(m.group(1)) if m.group(1) else 0
            mm = int(m.group(2))
            ss = int(m.group(3))
            state.eta_seconds = h * 3600 + mm * 60 + ss
            found = True
        except Exception:
            pass
    else:
        m = _RE_ETA_UNITS.search(line)
        if m and any(m.groups()):
            try:
                h = int(m.group(1)) if m.group(1) else 0
                mm = int(m.group(2)) if m.group(2) else 0
                ss = int(m.group(3)) if m.group(3) else 0
                if h or mm or ss:
                    state.eta_seconds = h * 3600 + mm * 60 + ss
                    found = True
            except Exception:
                pass

    # -- percentage (lowest-priority numeric signal) -------------------------
    m = _RE_PCT.search(line)
    if m:
        try:
            p = float(m.group(1))
            if 0.0 <= p <= 100.0:
                state._percent = p
                found = True
        except Exception:
            pass

    # -- current file -------------------------------------------------------
    # Prefer a token with a trusted extension; otherwise accept any file-like
    # token only when a context verb is present on the line.
    fm = None
    for cand in _RE_FILE_TOKEN.finditer(line):
        tok = cand.group("f")
        base = tok.replace("\\", "/").split("/")[-1]
        dot = base.rfind(".")
        ext = base[dot:].lower() if dot >= 0 else ""
        if ext in _FILE_EXT_ALLOW or _FILE_CONTEXT.search(low):
            fm = base
            if ext in _FILE_EXT_ALLOW:
                break
    if fm:
        state.current_file = fm
        found = True

    # -- phase words --------------------------------------------------------
    for needle, label in _PHASE_WORDS:
        if needle in low:
            state.stage = label
            break

    if found:
        state.indeterminate = False
        state.last_update = time.monotonic()
    return found


def split_progress_fragments(text: str):
    """Yield display fragments from a raw chunk that may use \\r (carriage-
    return progress overwrites) and/or \\n. Lazy/mkpfs redraw a live bar with
    \\r, so a naive readline() stalls until the whole stage ends. Splitting on
    both lets the caller pump each redraw to the parser as it arrives."""
    if not text:
        return
    buf = text.replace("\r\n", "\n")
    for piece in re.split(r"[\r\n]", buf):
        if piece:
            yield piece
