"""Transfer-queue model for the FTP tab (Phase 1).

Pure Python — no Tkinter, no ftplib — so it can be unit-tested headless and
reasoned about in isolation. The dispatcher/worker and all UI live in
exfat_builder.py / ui/tab_ftp.py and operate on this model via `self.after`.

Phase 1 scope only:
  - ordered queue of upload items
  - serial dispatch (one active at a time)
  - per-item state + progress + cancel flag
  - clear-completed (terminal states only)
No pause/resume, no persistence, no parallelism, no retry state machine.
"""

import os
import threading
import itertools


# Item states
QUEUED = "queued"
UPLOADING = "uploading"
DONE = "done"
FAILED = "failed"
CANCELLED = "cancelled"

TERMINAL_STATES = (DONE, FAILED, CANCELLED)

_id_counter = itertools.count(1)


class TransferItem(object):
    """One queued upload. A folder is a single item (aggregate progress),
    matching the approved Phase 1 decision."""

    __slots__ = ("id", "local_path", "remote_dir", "is_folder", "size",
                 "state", "sent", "speed_mbs", "error", "_cancel",
                 "name", "remote_display")

    def __init__(self, local_path, remote_dir, is_folder=False, size=0):
        self.id = next(_id_counter)
        self.local_path = local_path
        self.remote_dir = remote_dir
        self.is_folder = bool(is_folder)
        self.size = int(size or 0)
        self.state = QUEUED
        self.sent = 0
        self.speed_mbs = 0.0
        self.error = ""
        self._cancel = False
        # Display helpers (computed once)
        base = os.path.basename((local_path or "").rstrip("/\\")) or local_path
        self.name = base or "(unknown)"
        self.remote_display = remote_dir

    def request_cancel(self):
        self._cancel = True

    @property
    def cancel_requested(self):
        return self._cancel

    @property
    def is_terminal(self):
        return self.state in TERMINAL_STATES

    @property
    def percent(self):
        if self.size <= 0:
            return 0
        return min(100, int(self.sent * 100 / self.size))

    def to_dict(self):
        """Snapshot for the UI renderer (so the worker never hands live
        objects across threads beyond simple reads)."""
        return {
            "id": self.id, "name": self.name,
            "remote_display": self.remote_display,
            "is_folder": self.is_folder, "size": self.size,
            "state": self.state, "sent": self.sent,
            "percent": self.percent, "speed_mbs": self.speed_mbs,
            "error": self.error,
        }


class TransferQueue(object):
    """Ordered list of TransferItems with serial-dispatch helpers.

    The model itself does not spawn threads or talk to FTP; it only tracks
    state. The dispatcher (in the app) calls `next_queued()` and drives the
    transport, updating item fields as it goes.
    """

    def __init__(self):
        self._items = []
        self._lock = threading.Lock()
        self.active_id = None

    # ---- mutation -------------------------------------------------------
    def enqueue(self, item):
        with self._lock:
            self._items.append(item)
        return item

    def enqueue_many(self, items):
        with self._lock:
            self._items.extend(items)
        return list(items)

    def next_queued(self):
        """Return the first QUEUED item (not yet started), or None."""
        with self._lock:
            for it in self._items:
                if it.state == QUEUED:
                    return it
        return None

    def clear_completed(self):
        """Remove only terminal items (done/failed/cancelled). Keeps
        queued + uploading. Returns the number removed."""
        with self._lock:
            before = len(self._items)
            self._items = [it for it in self._items if not it.is_terminal]
            return before - len(self._items)

    def get(self, item_id):
        with self._lock:
            for it in self._items:
                if it.id == item_id:
                    return it
        return None

    # ---- read ------------------------------------------------------------
    def items(self):
        with self._lock:
            return list(self._items)

    def snapshot(self):
        with self._lock:
            return [it.to_dict() for it in self._items]

    def counts(self):
        with self._lock:
            c = {QUEUED: 0, UPLOADING: 0, DONE: 0, FAILED: 0, CANCELLED: 0}
            for it in self._items:
                c[it.state] = c.get(it.state, 0) + 1
            return c

    @property
    def has_pending(self):
        """True if any item is queued or uploading."""
        with self._lock:
            return any(it.state in (QUEUED, UPLOADING) for it in self._items)

    @property
    def is_empty(self):
        with self._lock:
            return not self._items
