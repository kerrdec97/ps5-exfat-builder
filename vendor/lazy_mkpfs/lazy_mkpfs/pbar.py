"""Progress / progress-bar helpers.
This module provides the Progress class used by CLI build flows.
"""
from __future__ import annotations
import sys
import time
from .utils import human_readable_size

class Progress:
    """Simple terminal progress helper used by CLI build flows.
    The Progress class writes progress updates to stderr. It is intentionally
    lightweight and has no external dependencies to keep CLI startup fast.
    """

    def __init__(self, enabled: bool = True, width: int = 32, update_interval: float = 0.1) -> None:
        self.enabled: bool = enabled
        self.width: int = width
        self.update_interval: float = update_interval
        
        # Phase tracking dictionaries
        self.phase_start_time: dict[str, float] = {}
        self.phase_bytes: dict[str, int] = {}
        self.phase_last_len: dict[str, int] = {}
        self.phase_last_update: dict[str, float] = {}
        
        # CRITICAL FIX: Track finished phases to prevent the "100% newline spam" bug
        self.finished_phases: set[str] = set()

    def step(self, phase: str, done: int, total: int, bytes_processed: int = 0) -> None:
        """Update progress for a named phase."""
        if not self.enabled:
            return

        # If this phase has already reached 100% and printed a newline, 
        # ignore further updates to prevent terminal spam.
        if phase in self.finished_phases:
            return

        now = time.time()
        last_update = self.phase_last_update.get(phase, 0.0)

        # Throttle updates to prevent terminal I/O lag, but always allow the final 100% update
        if now - last_update < self.update_interval and done < total:
            return

        self.phase_last_update[phase] = now

        # Initialize phase tracking if needed
        if phase not in self.phase_start_time:
            self.phase_start_time[phase] = now
            self.phase_bytes[phase] = 0

        if bytes_processed > 0:
            self.phase_bytes[phase] = bytes_processed

        total = max(total, 1)
        done = max(0, min(done, total))
        ratio: float = done / total
        fill: int = int(self.width * ratio)
        
        bar: str = "#" * fill + "-" * (self.width - fill)
        pct: int = int(ratio * 100)

        # Calculate speed and ETA
        elapsed: float = now - self.phase_start_time[phase]
        speed_str: str = ""
        eta_str: str = ""

        if elapsed > 0.1 and done > 0:
            if bytes_processed > 0:
                speed: float = self.phase_bytes[phase] / elapsed
                speed_str = f" @ {human_readable_size(int(speed))}/s"
                if done < total:
                    remaining_bytes: float = (self.phase_bytes[phase] / done) * (total - done)
                    eta_secs: float = remaining_bytes / speed if speed > 0 else 0
                    eta_str = f" | ETA {self._format_time(eta_secs)}"
            else:
                speed: float = done / elapsed
                speed_str = f" @ {speed:.1f} items/s"
                if done < total:
                    eta_secs: float = (total - done) / speed if speed > 0 else 0
                    eta_str = f" | ETA {self._format_time(eta_secs)}"

        line: str = f"[{bar}] {pct:3d}% {phase}{speed_str}{eta_str}"
        last_len: int = self.phase_last_len.get(phase, 0)
        padding: int = max(0, last_len - len(line))
        
        sys.stderr.write(f"\r{line}{' ' * padding}")
        sys.stderr.flush()
        self.phase_last_len[phase] = len(line)
        
        if done >= total:
            sys.stderr.write("\n")
            sys.stderr.flush()
            
            # Mark phase as finished so future calls are ignored
            self.finished_phases.add(phase)
            
            # Reset phase tracking to free memory
            self.phase_start_time.pop(phase, None)
            self.phase_bytes.pop(phase, None)
            self.phase_last_len.pop(phase, None)
            self.phase_last_update.pop(phase, None)

    def status(self, message: str) -> None:
        """Print a status message without progress bar.
        This clears the current progress bar line first to prevent overlapping.
        """
        if not self.enabled:
            return
            
        # Clear the current progress bar line if it exists
        max_len = max(self.phase_last_len.values(), default=0)
        if max_len > 0:
            sys.stderr.write(f"\r{' ' * (max_len + 10)}\r")
            sys.stderr.flush()
            
        sys.stderr.write(message + "\n")
        sys.stderr.flush()
        
        # Reset last lengths so the next progress bar doesn't leave trailing spaces
        self.phase_last_len.clear()

    @staticmethod
    def _format_time(seconds: float) -> str:
        """Format seconds into a clean, human-readable time string."""
        if seconds < 60:
            return f"{int(seconds)}s"
        elif seconds < 3600:
            return f"{int(seconds // 60)}m {int(seconds % 60):02d}s"
        else:
            h = int(seconds // 3600)
            m = int((seconds % 3600) // 60)
            return f"{h}h {m:02d}m"