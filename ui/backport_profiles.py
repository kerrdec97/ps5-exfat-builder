"""
ui/backport_profiles.py  —  Backport Profile Manager (manifest layer).

Additive safety layer over the existing backport engine (Option A). It does
NOT replace the legacy flat-zip restore (_abp_restore) — that stays available
as "Legacy Restore" for unmanaged backports. New managed backports get a
per-game manifest and a structured backup tree:

    BackportBackup/
      <TitleID>/
        manifest.json
        original_zips/
          originals.zip            (the clean/original files, hashed)
        patched_targets/
          4xx/   patched.zip        (optional cache of a built target)
          10xx/  patched.zip

The manifest records, for one game:
    schema_version, title_id, title, game_folder,
    tool_version, created/updated timestamps,
    current_target (or null = clean/restored),
    originals: { zip path, sha256, file list with per-file sha256 },
    targets:   { "<fw>": { built_at, tool_version, patched_zip?, ... } },
    history:   [ {action, target, timestamp, tool_version} ... ]

Non-negotiable safeguards implemented here:
  * Never trust a backup without verifying it (zip exists + sha256 matches).
  * restore_originals refuses if the originals zip is missing or its hash
    mismatches the manifest.
  * preview_changes performs a pure DRY-RUN (touches nothing) for every mode.
  * detect_state distinguishes managed (manifest present) vs unmanaged
    (legacy / no manifest) so the UI can require a clean source before
    target-switching an unmanaged backport.

This module is GUI-free and import-light (stdlib only) so it can be unit
tested headlessly. The actual patching is still done by the engine in
exfat_builder.py; the manager brackets that call with manifest + hashing.
"""

from __future__ import annotations

import os
import json
import time
import hashlib
import zipfile
import tempfile

SCHEMA_VERSION = 1

# Files the backport engine patches (mirrors the engine's own selection so a
# snapshot covers exactly what may be modified).
PATCH_EXTS = (".self", ".sprx", ".prx", ".elf", ".bin")

MANIFEST_NAME = "manifest.json"
ORIGINALS_DIRNAME = "original_zips"
ORIGINALS_ZIP = "originals.zip"
TARGETS_DIRNAME = "patched_targets"


# ---------------------------------------------------------------------------
def normalize_fw(fw) -> str:
    """Normalize a firmware/target label to a compact key like '4xx' / '10xx'.

    Accepts '4', '4.xx', '4xx', 4, '10.50', etc. We key by the MAJOR pair the
    SDK patcher uses (1-10), so '4.03' and '4.50' both map to '4xx'.
    """
    s = str(fw).strip().lower().replace(".xx", "").replace("xx", "")
    # keep leading digits only (major version)
    digits = ""
    for ch in s:
        if ch.isdigit():
            digits += ch
        else:
            break
    if not digits:
        # fall back: any digits at all
        for ch in s:
            if ch.isdigit():
                digits += ch
    if not digits:
        return str(fw).strip()
    return digits + "xx"


def sha256_file(path, _buf=1024 * 1024) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(_buf)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def backup_root_for(game_folder, title_id, base_dir=None) -> str:
    """Return BackportBackup/<TitleID>/ for a game.

    base_dir defaults to a 'BackportBackup' folder beside the game folder's
    parent (so it sits next to the game, not inside it). title_id falls back
    to the folder basename when unknown.
    """
    tid = (title_id or "").strip() or os.path.basename(
        os.path.normpath(game_folder)) or "UNKNOWN"
    # sanitise for a folder name
    tid = "".join(c for c in tid if c.isalnum() or c in ("-", "_")) or "UNKNOWN"
    if base_dir:
        root = base_dir
    else:
        parent = os.path.dirname(os.path.normpath(game_folder))
        root = os.path.join(parent, "BackportBackup")
    return os.path.join(root, tid)


def manifest_path_for(game_folder, title_id, base_dir=None) -> str:
    return os.path.join(backup_root_for(game_folder, title_id, base_dir),
                        MANIFEST_NAME)


# ---------------------------------------------------------------------------
def read_manifest(path) -> dict | None:
    try:
        if not path or not os.path.isfile(path):
            return None
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            return None
        return data
    except Exception:
        return None


def write_manifest(path, manifest) -> bool:
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # atomic-ish write: temp file in the same dir, then replace
        d = os.path.dirname(path)
        fd, tmp = tempfile.mkstemp(prefix=".manifest_", dir=d)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(manifest, f, indent=2, sort_keys=False)
            os.replace(tmp, path)
        finally:
            if os.path.exists(tmp):
                try:
                    os.remove(tmp)
                except Exception:
                    pass
        return True
    except Exception:
        return False


def new_manifest(game_folder, title_id, title, tool_version) -> dict:
    now = _now()
    return {
        "schema_version": SCHEMA_VERSION,
        "title_id": title_id or "",
        "title": title or "",
        "game_folder": os.path.normpath(game_folder),
        "tool_version": tool_version,
        "created_at": now,
        "updated_at": now,
        "current_target": None,
        "originals": None,
        "targets": {},
        "history": [],
    }


def _now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def _add_history(manifest, action, target=None):
    manifest.setdefault("history", []).append({
        "action": action,
        "target": target,
        "timestamp": _now(),
        "tool_version": manifest.get("tool_version", ""),
    })
    manifest["updated_at"] = _now()


# ---------------------------------------------------------------------------
def list_patchable_files(game_folder) -> list:
    """Relative paths (posix-style) of files the engine would patch."""
    out = []
    for root, _dirs, files in os.walk(game_folder):
        for fn in files:
            if fn.lower().endswith(PATCH_EXTS):
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, game_folder).replace(os.sep, "/")
                out.append(rel)
    return sorted(out)


def snapshot_originals(manifest, game_folder, base_dir=None,
                       extra_rel_files=None) -> dict:
    """Zip the current (assumed-clean) patchable files into originals.zip and
    record per-file + zip sha256 in the manifest. Returns the manifest.

    extra_rel_files: additional relative paths to include (e.g. fakelib files
    the engine will add) so a later restore can also remove/replace them.
    """
    root = backup_root_for(game_folder, manifest.get("title_id"), base_dir)
    odir = os.path.join(root, ORIGINALS_DIRNAME)
    os.makedirs(odir, exist_ok=True)
    ozip = os.path.join(odir, ORIGINALS_ZIP)

    rels = list_patchable_files(game_folder)
    if extra_rel_files:
        for r in extra_rel_files:
            r = r.replace(os.sep, "/")
            if r not in rels and os.path.isfile(os.path.join(game_folder, r)):
                rels.append(r)
    rels = sorted(set(rels))

    file_hashes = {}
    with zipfile.ZipFile(ozip, "w", zipfile.ZIP_DEFLATED) as zf:
        for rel in rels:
            full = os.path.join(game_folder, rel.replace("/", os.sep))
            if not os.path.isfile(full):
                continue
            zf.write(full, rel)
            file_hashes[rel] = sha256_file(full)

    manifest["originals"] = {
        "zip": os.path.relpath(ozip, root).replace(os.sep, "/"),
        "zip_abs": ozip,
        "zip_sha256": sha256_file(ozip),
        "files": file_hashes,
        "snapshot_at": _now(),
    }
    _add_history(manifest, "snapshot_originals")
    return manifest


# ---------------------------------------------------------------------------
def verify_backup(manifest, base_dir=None) -> tuple:
    """Verify the originals backup is present and intact.

    Returns (ok: bool, reason: str). ok=False means restore MUST be refused.
    """
    o = (manifest or {}).get("originals")
    if not o:
        return False, "No originals snapshot recorded in manifest."
    game_folder = manifest.get("game_folder", "")
    root = backup_root_for(game_folder, manifest.get("title_id"), base_dir)
    ozip = o.get("zip_abs") or os.path.join(
        root, o.get("zip", "").replace("/", os.sep))
    if not ozip or not os.path.isfile(ozip):
        return False, "Originals backup zip is missing: %s" % ozip
    try:
        actual = sha256_file(ozip)
    except Exception as e:
        return False, "Could not hash originals zip: %s" % e
    if actual != o.get("zip_sha256"):
        return False, ("Originals backup zip hash mismatch (corrupt or "
                       "modified). Expected "
                       f"{o.get('zip_sha256', '?')[:12]!s}, got {actual[:12]!s}.")
    return True, "Originals backup present and hash-verified."


def verify_clean(manifest, game_folder) -> tuple:
    """After a restore, confirm the on-disk patchable files match the recorded
    original hashes. Returns (ok, reason, mismatched_list)."""
    o = (manifest or {}).get("originals") or {}
    recorded = o.get("files") or {}
    if not recorded:
        return False, "No per-file original hashes recorded.", []
    mismatched = []
    for rel, want in recorded.items():
        full = os.path.join(game_folder, rel.replace("/", os.sep))
        if not os.path.isfile(full):
            mismatched.append(rel + " (missing)")
            continue
        try:
            got = sha256_file(full)
        except Exception:
            mismatched.append(rel + " (unreadable)")
            continue
        if got != want:
            mismatched.append(rel)
    if mismatched:
        return (False, "%d file(s) do not match clean originals." %
                len(mismatched), mismatched)
    return True, "On-disk files match clean originals.", []


# ---------------------------------------------------------------------------
def restore_originals(manifest, game_folder, base_dir=None,
                      log=None) -> tuple:
    """Hash-verified restore of originals into the game folder.

    REFUSES (returns ok=False) if verify_backup fails. Returns
    (ok, reason, restored_count).
    """
    def _log(m):
        if log:
            try:
                log(m)
            except Exception:
                pass

    ok, reason = verify_backup(manifest, base_dir)
    if not ok:
        _log("[BPM] Restore refused: %s\n" % reason)
        return False, reason, 0

    o = manifest["originals"]
    root = backup_root_for(game_folder, manifest.get("title_id"), base_dir)
    ozip = o.get("zip_abs") or os.path.join(
        root, o.get("zip", "").replace("/", os.sep))

    restored = 0
    try:
        with zipfile.ZipFile(ozip, "r") as zf:
            for member in zf.infolist():
                dst = os.path.join(game_folder,
                                   member.filename.replace("/", os.sep))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with zf.open(member) as src_f, open(dst, "wb") as dst_f:
                    while True:
                        chunk = src_f.read(1024 * 1024)
                        if not chunk:
                            break
                        dst_f.write(chunk)
                restored += 1
    except Exception as e:
        _log("[BPM] Restore error: %s\n" % e)
        return False, "Restore failed: %s" % e, restored

    manifest["current_target"] = None
    _add_history(manifest, "restore_originals")
    _log("[BPM] Restored %d original file(s) from verified backup.\n"
         % restored)
    return True, "Restored %d file(s)." % restored, restored


# ---------------------------------------------------------------------------
def detect_state(game_folder, title_id=None, base_dir=None) -> dict:
    """Inspect a game folder and report a managed/unmanaged state summary for
    the UI. Pure read — touches nothing.

    Returns dict:
        managed: bool
        manifest_path: str|None
        current_target: str|None
        originals_present: bool
        backup_verified: bool
        verify_reason: str
        title_id, title
    """
    mpath = manifest_path_for(game_folder, title_id, base_dir)
    manifest = read_manifest(mpath)
    if not manifest:
        return {
            "managed": False,
            "manifest_path": None,
            "current_target": None,
            "originals_present": False,
            "backup_verified": False,
            "verify_reason": "No manifest found — unmanaged backport.",
            "title_id": title_id or "",
            "title": "",
        }
    ok, reason = verify_backup(manifest, base_dir)
    return {
        "managed": True,
        "manifest_path": mpath,
        "current_target": manifest.get("current_target"),
        "originals_present": bool(manifest.get("originals")),
        "backup_verified": ok,
        "verify_reason": reason,
        "title_id": manifest.get("title_id", ""),
        "title": manifest.get("title", ""),
        "targets": sorted((manifest.get("targets") or {}).keys()),
    }


# ---------------------------------------------------------------------------
def preview_changes(mode, game_folder, title_id=None, new_target=None,
                    base_dir=None) -> dict:
    """Pure dry-run. Returns a plan dict describing what WOULD happen, plus a
    'safe' flag and any 'blockers'. Touches nothing on disk.

    mode: 'build' | 'swap' | 'remove'
    """
    state = detect_state(game_folder, title_id, base_dir)
    plan = {
        "mode": mode,
        "new_target": normalize_fw(new_target) if new_target else None,
        "state": state,
        "steps": [],
        "blockers": [],
        "safe": True,
    }

    patchable = list_patchable_files(game_folder)
    n = len(patchable)

    if mode == "build":
        # Build = snapshot originals + patch to target
        if state["managed"] and state["current_target"]:
            plan["blockers"].append(
                "This game already has a managed backport for target %s. Use Swap "
                "(restore then re-patch) instead of Build, to avoid patching over patched files."
                % state["current_target"])
            plan["safe"] = False
        if not plan["new_target"]:
            plan["blockers"].append("No target firmware selected.")
            plan["safe"] = False
        plan["steps"] = [
            "Snapshot %d patchable file(s) into originals.zip (with hashes)"
            % n,
            "Write manifest (target=%s)" % plan["new_target"],
            "Patch %d file(s) to target %s via the backport engine"
            % (n, plan["new_target"]),
            "Update manifest current_target=%s" % plan["new_target"],
        ]

    elif mode == "swap":
        if not state["managed"]:
            plan["blockers"].append(
                "Unmanaged backport (no manifest). Cannot safely swap targets — "
                "start from a clean source and Build instead, or use Legacy "
                "Restore first.")
            plan["safe"] = False
        else:
            ok, reason = verify_backup(read_manifest(state["manifest_path"]),
                                       base_dir)
            if not ok:
                plan["blockers"].append("Backup verification failed: " + reason)
                plan["safe"] = False
        if not plan["new_target"]:
            plan["blockers"].append("No new target firmware selected.")
            plan["safe"] = False
        elif state.get("current_target") and \
                normalize_fw(state["current_target"]) == plan["new_target"]:
            plan["blockers"].append(
                "Game is already on target %s." % plan["new_target"])
            plan["safe"] = False
        plan["steps"] = [
            "Verify originals backup (hash check) — refuse if missing/corrupt",
            "Restore %d original file(s) from verified backup" % n,
            "Verify on-disk files match clean originals (hash check)",
            "Patch to NEW target %s via the backport engine"
            % plan["new_target"],
            "Update manifest current_target=%s" % plan["new_target"],
        ]

    elif mode == "remove":
        if not state["managed"]:
            plan["blockers"].append(
                "Unmanaged backport (no manifest). Use Legacy Restore to pick "
                "an originals zip manually.")
            plan["safe"] = False
        else:
            ok, reason = verify_backup(read_manifest(state["manifest_path"]),
                                       base_dir)
            if not ok:
                plan["blockers"].append("Backup verification failed: " + reason)
                plan["safe"] = False
        plan["steps"] = [
            "Verify originals backup (hash check) — refuse if missing/corrupt",
            "Restore %d original file(s) from verified backup" % n,
            "Verify on-disk files match clean originals (hash check)",
            "Clear managed patched state (manifest current_target=None)",
        ]

    else:
        plan["blockers"].append("Unknown mode: %s" % mode)
        plan["safe"] = False

    return plan


# ---------------------------------------------------------------------------
def record_target_built(manifest, fw, tool_version,
                        post_hashes=None) -> dict:
    """Mark that the game is now patched to `fw`. post_hashes is an optional
    rel->sha256 map of the patched files (the 'after' hashes)."""
    key = normalize_fw(fw)
    manifest.setdefault("targets", {})[key] = {
        "built_at": _now(),
        "tool_version": tool_version,
        "post_hashes": post_hashes or {},
    }
    manifest["current_target"] = key
    _add_history(manifest, "apply_target", key)
    return manifest
