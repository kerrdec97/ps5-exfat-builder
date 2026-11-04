#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# Third-party code: ampr_emu, tools/build_ampr_index.py
#
#   Original author: drakmor (Roman Tarasov)
#   Upstream:        https://github.com/drakmor/ampr_emu
#                    file tools/build_ampr_index.py
#   Upstream commit: 6c8d669eef6cd06be0504de3bd7554b1597bc916 (tag 0.3.1,
#                    2026-08-16). The copy first committed here (v3.6.4) is
#                    byte-identical to the file at that commit (git blob
#                    6083ec49cfa5b8363bd4fae54443b12da640df86). Upstream
#                    changed the file later (0.3.4, 0.3.6). Those changes are
#                    not in this copy. Compared with upstream HEAD cfa85df
#                    (2026-09-06).
#
#   Licence: GNU General Public License, version 3 (GPL-3.0). This is the
#   licence of the upstream repository: its LICENSE file is the GPL v3 text
#   (checked at 6c8d669 and cfa85df). The upstream file has no licence
#   notice of its own.
#
#   This program is free software: you can redistribute it and/or modify it
#   under the terms of the GNU General Public License as published by the
#   Free Software Foundation, version 3. It is distributed WITHOUT ANY
#   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
#   FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
#   details. The full text is in LICENSES/GPL-3.0.txt. See also
#   THIRD_PARTY_NOTICES.md.
#
#   Local modifications (exFAT Image Builder, between v3.6.4 and v4.0.2):
#     - build_index_local(), build_index_ftp() and collect_ftp_rows() take an
#       optional progress= callback. The UI uses it to show scan, build and
#       upload status. Calls are throttled with time.monotonic(), so the
#       module imports time.
#     - build_index_local() and build_index_ftp() take an optional
#       stats_out= dict. It receives the folder and file counts.
#     - This header.
#   Both new parameters default to None. With the defaults, the functions and
#   the command line behave as upstream 0.3.1 and write the same index bytes.
# ---------------------------------------------------------------------------
"""
Build /app0/ampr_emu.index for the AMPR APR file resolver.

The index is a compact binary file:

    AMPRIDX3 header
    fixed-size sorted records
    NUL-terminated /app0 path blob
    prebuilt open-addressed hash slots

Paths are stored with forward slashes and matched case-insensitively by the PRX.
The input root can be either a local directory or ftp://host[:port]/path/to/app0.
"""

from __future__ import annotations

import argparse
import ftplib
import os
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import unquote, urlparse


@dataclass
class HashCollisionStats:
    probe_steps: int = 0
    probed_entries: int = 0
    max_probe: int = 0
    duplicate_hash_groups: int = 0
    duplicate_hash_entries: int = 0
    duplicate_hash_samples: list[tuple[int, str, str]] = field(default_factory=list)


def key_for(path: str) -> str:
    return path.replace("\\", "/").lower()


def fnv1a64_path_hash(path: str) -> int:
    h = 1469598103934665603
    for ch in key_for(path):
        h ^= ord(ch)
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h or 1


def hash_slot_count(entry_count: int) -> int:
    if entry_count <= 0 or entry_count > 0xFFFFFFFE:
        raise ValueError("invalid index entry count")
    slots = 2
    target = entry_count * 2
    while slots < target:
        slots <<= 1
    return slots


def build_hash_slots(rows: list[tuple[int, int, str]]) -> tuple[list[tuple[int, int, int]], HashCollisionStats]:
    duplicate_flag = 1
    slots = [(0, 0, 0) for _ in range(hash_slot_count(len(rows)))]
    mask = len(slots) - 1
    stats = HashCollisionStats()
    duplicate_hashes: set[int] = set()
    for index, (_, _, path) in enumerate(rows):
        h = fnv1a64_path_hash(path)
        pos = h & mask
        duplicate = False
        probe = 0
        while slots[pos][1] != 0:
            if slots[pos][0] == h:
                old_hash, old_index_plus_one, old_flags = slots[pos]
                slots[pos] = (old_hash, old_index_plus_one, old_flags | duplicate_flag)
                if not duplicate:
                    if h not in duplicate_hashes:
                        duplicate_hashes.add(h)
                        stats.duplicate_hash_groups += 1
                        stats.duplicate_hash_entries += 2
                    else:
                        stats.duplicate_hash_entries += 1
                    if len(stats.duplicate_hash_samples) < 5:
                        stats.duplicate_hash_samples.append((h, rows[old_index_plus_one - 1][2], path))
                duplicate = True
            pos = (pos + 1) & mask
            probe += 1
        if probe:
            stats.probed_entries += 1
            stats.probe_steps += probe
            stats.max_probe = max(stats.max_probe, probe)
        slots[pos] = (h, index + 1, duplicate_flag if duplicate else 0)
    return slots, stats


def report_hash_collision_stats(stats: HashCollisionStats, slot_count: int, entry_count: int) -> None:
    if stats.probe_steps:
        print(
            "info: AMPRIDX3 hash table probe stats: "
            f"entries={entry_count} slots={slot_count} "
            f"probedEntries={stats.probed_entries} "
            f"probeSteps={stats.probe_steps} maxProbe={stats.max_probe}",
            file=sys.stderr,
        )
    if stats.duplicate_hash_groups:
        print(
            "warning: AMPRIDX3 duplicate 64-bit path hashes: "
            f"groups={stats.duplicate_hash_groups} entries={stats.duplicate_hash_entries}; "
            "duplicate slots will force full path compare at runtime",
            file=sys.stderr,
        )
        for h, first, second in stats.duplicate_hash_samples:
            print(
                f"warning: AMPRIDX3 duplicate hash sample hash=0x{h:016x}: {first} <-> {second}",
                file=sys.stderr,
            )


def app0_path(root: Path, path: Path) -> str:
    rel = path.relative_to(root).as_posix()
    return "/app0/" + rel


def report_progress(count: int) -> None:
    if count and count % 10000 == 0:
        print(f"indexed {count} files...", flush=True)


def validate_and_add_row(
    rows: list[tuple[int, int, str]],
    seen: dict[str, str],
    size: int,
    mtime: int,
    indexed_path: str,
    allow_case_collisions: bool,
) -> bool:
    if "\t" in indexed_path or "\n" in indexed_path or "\r" in indexed_path:
        print(f"warning: skipping path with unsupported whitespace: {indexed_path}", file=sys.stderr)
        return True

    key = key_for(indexed_path)
    existing = seen.get(key)
    if existing is not None:
        msg = f"case-insensitive path collision: {existing} <-> {indexed_path}"
        if not allow_case_collisions:
            print(f"error: {msg}", file=sys.stderr)
            return False
        print(f"warning: keeping first collision entry: {msg}", file=sys.stderr)
        return True

    seen[key] = indexed_path
    rows.append((size, mtime, indexed_path))
    report_progress(len(rows))
    return True


def write_index(rows: list[tuple[int, int, str]], output: Path) -> None:
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp = output.with_suffix(output.suffix + ".tmp")
    rows = sorted(rows, key=lambda row: key_for(row[2]))

    record_struct = struct.Struct("<IIQq")
    hash_slot_struct = struct.Struct("<QII")
    header_struct = struct.Struct("<8sIIQQQII")
    if len(rows) > 0xFFFFFFFE:
        raise ValueError("index has too many records")
    path_blob = bytearray()
    records = bytearray()
    for size, mtime, path in rows:
        encoded = path.encode("utf-8") + b"\0"
        offset = len(path_blob)
        path_len = len(encoded) - 1
        if offset > 0xFFFFFFFF or path_len > 0xFFFFFFFF:
            raise ValueError("index path blob is too large")
        records += record_struct.pack(offset, path_len, size, mtime)
        path_blob += encoded

    hash_slots, hash_stats = build_hash_slots(rows)
    report_hash_collision_stats(hash_stats, len(hash_slots), len(rows))
    path_end = header_struct.size + len(records) + len(path_blob)
    hash_offset = (path_end + (hash_slot_struct.size - 1)) & ~(hash_slot_struct.size - 1)
    padding = b"\0" * (hash_offset - path_end)

    with tmp.open("wb") as f:
        f.write(
            header_struct.pack(
                b"AMPRIDX3",
                3,
                record_struct.size,
                len(rows),
                len(path_blob),
                hash_offset,
                hash_slot_struct.size,
                len(hash_slots),
            )
        )
        f.write(records)
        f.write(path_blob)
        f.write(padding)
        for h, index_plus_one, flags in hash_slots:
            f.write(hash_slot_struct.pack(h, index_plus_one, flags))
    tmp.replace(output)


def build_index_local(root: Path, output: Path, allow_case_collisions: bool, progress=None, stats_out=None) -> int:
    root = root.resolve()
    if not root.is_dir():
        print(f"error: root is not a directory: {root}", file=sys.stderr)
        return 2

    output = output.resolve()
    output_tmp = output.with_suffix(output.suffix + ".tmp")
    seen: dict[str, str] = {}
    rows: list[tuple[int, int, str]] = []
    dirs_seen = 0
    last_emit = 0.0

    for dirpath, dirnames, filenames in os.walk(root):
        dirs_seen += 1
        if progress is not None:
            now = time.monotonic()
            if now - last_emit >= 0.5 or dirs_seen % 200 == 0:
                last_emit = now
                progress(f"Scanning files ({dirs_seen} folders, {len(rows)} files)")
        dirnames.sort(key=str.lower)
        filenames.sort(key=str.lower)
        for filename in filenames:
            path = Path(dirpath) / filename
            try:
                resolved = path.resolve()
            except OSError as exc:
                print(f"warning: skipping unresolved path {path}: {exc}", file=sys.stderr)
                continue
            if resolved == output or resolved == output_tmp:
                continue
            indexed_path = app0_path(root, path)
            try:
                st = path.stat()
            except OSError as exc:
                print(f"warning: skipping unreadable file {indexed_path}: {exc}", file=sys.stderr)
                continue
            if not path.is_file():
                continue
            if not validate_and_add_row(
                rows,
                seen,
                st.st_size,
                int(st.st_mtime),
                indexed_path,
                allow_case_collisions,
            ):
                return 3

    if stats_out is not None:
        stats_out["folders"] = dirs_seen
        stats_out["files"] = len(rows)
    if progress is not None:
        progress(f"Building index ({len(rows)} files)…")
    write_index(rows, output)
    print(f"indexed {len(rows)} files from {root}")
    print(f"wrote {output}")
    return 0


def is_ftp_url(root: str) -> bool:
    return urlparse(root).scheme.lower() == "ftp"


def ftp_join(parent: str, name: str) -> str:
    parent = parent.rstrip("/")
    return f"{parent}/{name}" if parent else f"/{name}"


def ftp_modify_to_int(value: str) -> int:
    # MLSD modify is UTC YYYYMMDDHHMMSS. The emulator only uses mtime as
    # cached metadata, so the compact integer is stable and timezone-free.
    if len(value) >= 14 and value[:14].isdigit():
        return int(value[:14])
    return 0


def ftp_is_dir(ftp: ftplib.FTP, path: str) -> bool:
    old = ftp.pwd()
    try:
        ftp.cwd(path)
        ftp.cwd(old)
        return True
    except ftplib.all_errors:
        try:
            ftp.cwd(old)
        except ftplib.all_errors:
            pass
        return False


def ftp_file_facts(ftp: ftplib.FTP, path: str) -> tuple[int, int]:
    size = 0
    mtime = 0
    try:
        got_size = ftp.size(path)
        if got_size is not None:
            size = int(got_size)
    except ftplib.all_errors:
        pass
    try:
        resp = ftp.sendcmd(f"MDTM {path}")
        parts = resp.split(maxsplit=1)
        if len(parts) == 2:
            mtime = ftp_modify_to_int(parts[1].strip())
    except ftplib.all_errors:
        pass
    return size, mtime


def ftp_list_entries(ftp: ftplib.FTP, current: str) -> list[tuple[str, dict[str, str]]]:
    try:
        return list(ftp.mlsd(current))
    except ftplib.all_errors:
        pass

    names = ftp.nlst(current)
    result: list[tuple[str, dict[str, str]]] = []
    prefix = current.rstrip("/") + "/"
    for item in names:
        name = item[len(prefix):] if item.startswith(prefix) else item.rsplit("/", 1)[-1]
        if name in ("", ".", ".."):
            continue
        remote_path = ftp_join(current, name)
        if ftp_is_dir(ftp, remote_path):
            result.append((name, {"type": "dir"}))
        else:
            size, mtime = ftp_file_facts(ftp, remote_path)
            result.append((name, {"type": "file", "size": str(size), "modify": str(mtime)}))
    return result


def parse_ftp_root(root_url: str) -> tuple[str, int, str, str, str]:
    parsed = urlparse(root_url)
    if not parsed.hostname:
        raise ValueError(f"FTP URL has no host: {root_url}")
    host = parsed.hostname
    port = parsed.port or 21
    user = unquote(parsed.username) if parsed.username else "anonymous"
    password = unquote(parsed.password) if parsed.password else "anonymous@"
    root = unquote(parsed.path or "/")
    if not root.startswith("/"):
        root = "/" + root
    root = root.rstrip("/") or "/"
    return host, port, user, password, root


def collect_ftp_rows(
    ftp: ftplib.FTP,
    root: str,
    allow_case_collisions: bool,
    progress=None,
) -> tuple[int, list[tuple[int, int, str]]]:
    seen: dict[str, str] = {}
    rows: list[tuple[int, int, str]] = []
    dirs_seen = 0
    stack = [root]
    last_emit = 0.0
    while stack:
        current = stack.pop()
        dirs_seen += 1
        if progress is not None:
            now = time.monotonic()
            if now - last_emit >= 1.0 or dirs_seen % 100 == 0:
                last_emit = now
                progress(f"Scanning files ({dirs_seen} folders, {len(rows)} files)")
        try:
            entries = ftp_list_entries(ftp, current)
        except ftplib.error_perm as exc:
            print(f"warning: skipping unreadable FTP directory {current}: {exc}", file=sys.stderr)
            continue
        entries.sort(key=lambda item: item[0].lower())

        child_dirs: list[str] = []
        for name, facts in entries:
            if name in (".", ".."):
                continue
            typ = facts.get("type", "").lower()
            remote_path = ftp_join(current, name)
            if typ == "dir":
                child_dirs.append(remote_path)
                continue
            if typ not in ("file", ""):
                continue

            rel = remote_path[len(root):].lstrip("/") if root != "/" else remote_path.lstrip("/")
            indexed_path = "/app0/" + rel.replace("\\", "/")
            indexed_key = key_for(indexed_path)
            if indexed_key in ("/app0/ampr_emu.index", "/app0/ampr_emu.index.tmp"):
                continue

            size = int(facts.get("size", "0") or "0")
            mtime = ftp_modify_to_int(facts.get("modify", ""))
            if not validate_and_add_row(rows, seen, size, mtime, indexed_path, allow_case_collisions):
                raise ValueError("case-insensitive path collision")
        stack.extend(reversed(child_dirs))
    return dirs_seen, rows


def upload_index_to_ftp(ftp: ftplib.FTP, root: str, output: Path) -> None:
    remote_tmp = ftp_join(root, "ampr_emu.index.tmp")
    remote_dst = ftp_join(root, "ampr_emu.index")
    with output.open("rb") as f:
        ftp.storbinary(f"STOR {remote_tmp}", f)
    try:
        ftp.delete(remote_dst)
    except ftplib.all_errors:
        pass
    ftp.rename(remote_tmp, remote_dst)
    print(f"uploaded {remote_dst}")


def build_index_ftp(root_url: str, output: Path, allow_case_collisions: bool, upload: bool, progress=None, stats_out=None) -> int:
    ftp: ftplib.FTP | None = None
    try:
        host, port, user, password, root = parse_ftp_root(root_url)
        if progress is not None:
            progress("Connecting to PS5…")
        ftp = ftplib.FTP()
        ftp.connect(host, port, timeout=30)
        ftp.login(user, password)

        if progress is not None:
            progress("Reading directory…")
        dirs_seen, rows = collect_ftp_rows(ftp, root, allow_case_collisions, progress)
        if stats_out is not None:
            stats_out["folders"] = dirs_seen
            stats_out["files"] = len(rows)
        if progress is not None:
            progress(f"Building index ({len(rows)} files)…")
        write_index(rows, output)
        print(f"indexed {len(rows)} files from {root_url} ({dirs_seen} directories)")
        print(f"wrote {output.resolve()}")
        if upload:
            if progress is not None:
                progress("Uploading to PS5…")
            upload_index_to_ftp(ftp, root, output.resolve())
        return 0
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    finally:
        if ftp is not None:
            try:
                ftp.quit()
            except ftplib.all_errors:
                ftp.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Build AMPR /app0 case-insensitive file index")
    parser.add_argument("root", help="local directory or ftp://[user[:pass]@]host[:port]/path mounted as /app0")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="index output path; default is <local-root>/ampr_emu.index or ./ampr_emu.index for FTP",
    )
    parser.add_argument(
        "--upload",
        action="store_true",
        help="with FTP root, upload the generated index to <ftp-root>/ampr_emu.index",
    )
    parser.add_argument(
        "--allow-case-collisions",
        action="store_true",
        help="do not fail when two files differ only by case; keep the first sorted entry",
    )
    args = parser.parse_args()

    if is_ftp_url(args.root):
        output = args.output if args.output else Path("ampr_emu.index")
        return build_index_ftp(args.root, output, args.allow_case_collisions, args.upload)

    if args.upload:
        print("error: --upload is only valid for FTP roots", file=sys.stderr)
        return 2
    root = Path(args.root)
    output = args.output if args.output else (root / "ampr_emu.index")
    return build_index_local(root, output, args.allow_case_collisions)


if __name__ == "__main__":
    raise SystemExit(main())
