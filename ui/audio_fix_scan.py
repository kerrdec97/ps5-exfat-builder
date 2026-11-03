"""
ui/audio_fix_scan.py  —  Audio Fix Candidate Scanner (Phase 1, READ-ONLY).

REVISED: raw whole-file byte scanning with OPTIONAL ELF/fSELF enrichment.

Why the revision: PS5 backport binaries (eboot.bin, .prx, .sprx) are usually
SCE fake-SELF (fSELF) containers — magic 4F 15 3D 1D — whose body is a
PLAINTEXT ELF. The previous version treated that magic as "encrypted" and
skipped the file, so a real backport scanned 8 files / 8 skipped / 0 candidates
— which proved "nothing readable under the old rules", NOT "no candidates".
The manual audio fix is a raw whole-file byte edit at fixed offsets
(0x42A029 / 0x497DD1 / 0x49809B), so the correct unit is the RAW FILE, not ELF
sections.

This scanner now:
  * scans RAW file bytes for the full 4-byte field "?? F1 63 2D" (never a lone
    leading byte);
  * classifies the file as ELF / SELF_OR_FSELF / NON_ELF / UNREADABLE, but does
    NOT skip on SELF magic — SELF/fSELF files are raw-scanned and reported with
    section "not resolved";
  * attaches ELF section info as OPTIONAL context only when a clean ELF is
    parseable; section info is NEVER required for a candidate;
  * derives confidence primarily from the KNOWN-OFFSET allowlist + full-field
    match, with an over-match ceiling.

It does NOT patch, write, back up, disassemble, or decode instructions, and
never claims to fix audio. Output is phrased as "candidate audio-fix data
fields". The ONLY skip reasons now are UNREADABLE files.

GUI-free, stdlib-only, unit-testable headlessly.
"""

from __future__ import annotations

import os
import json
import struct

# The 4-byte field: ?? F1 63 2D ; the leading byte's bit 2 selects 12 (0C) / 8 (08)
_FIELD_TAIL = bytes([0xF1, 0x63, 0x2D])
_FIELD_LEN = 4
_PATCH_BIT = 0x04
# Known manual-fix offsets (raw whole-file). HIGH confidence only here.
KNOWN_OFFSETS = frozenset({0x42A029, 0x497DD1, 0x49809B})

# Profiles are structural shapes (relative offsets) known from a confirmed
# build. The structure may relocate by a constant delta in another build of
# the same title; see validate_profile_relative. A profile match is NEVER a
# fix claim — it is evidence that the SAME STRUCTURE exists in this build.
# Offsets are kept absolute here; the validator works with differences.
PROFILES = {
    "A": {
        "name": "F1 audio-fix profile A",
        "offsets": sorted(KNOWN_OFFSETS),
    },
}
# Tolerance (bytes) for a "near" structural match. Data fields can shift by a
# few bytes between builds without changing the structure; anything larger
# is treated as not-relocated.
PROFILE_NEAR_TOL = 4

# Confidence tiers
CONF_HIGH = "HIGH"
CONF_STRUCTURAL = "STRUCTURAL"
CONF_MEDIUM = "MEDIUM"
CONF_LOW = "LOW"
CONF_SKIP = "SKIP"

DEFAULT_OVERMATCH_CEILING = 8

_CONTEXT_BEFORE = 9
_CONTEXT_AFTER = 7

# Container magics
_ELF_MAGIC = b"\x7fELF"
_SELF_MAGIC = bytes([0x4F, 0x15, 0x3D, 0x1D])

_DEFAULT_TARGETS = ("eboot.bin",)
_DEFAULT_DIRS = ("prx", "sce_module", "fakelib")
_WHOLE_FOLDER_EXTS = (".bin", ".prx", ".sprx")

# ELF section flags / types (informational only)
_SHF_WRITE = 0x1
_SHF_ALLOC = 0x2
_SHF_EXECINSTR = 0x4
_SHT_NOBITS = 8

# Sections where a static data field is *expected* (informational only)
_EXPECTED_DATA_SECTIONS = (".rodata", ".data", ".data.rel.ro", ".sdata")

# File format classification
FMT_ELF = "ELF"
FMT_SELF = "SELF_OR_FSELF"
FMT_NON_ELF = "NON_ELF"
FMT_UNREADABLE = "UNREADABLE"


def _find_embedded_elf_offset(data: bytes, search_limit=0x20000):
    """In a SELF/fSELF container, locate the embedded plaintext ELF header.

    Returns the offset of the '\\x7fELF' within the first `search_limit` bytes,
    or -1. fSELF wraps a plaintext ELF behind SCE/SELF headers; finding it lets
    us OPTIONALLY parse sections for context. Never required for scanning.
    """
    try:
        idx = data.find(_ELF_MAGIC, 0, search_limit)
        return idx
    except Exception:
        return -1


def _parse_elf_sections(data: bytes, elf_base=0):
    """Parse ELF64 section headers starting at `elf_base`. Returns [] if not
    parseable. Section offsets are returned as WHOLE-FILE offsets (elf_base
    added), so they can be compared against raw field offsets. Read-only."""
    try:
        if len(data) < elf_base + 64 or data[elf_base:elf_base + 4] != _ELF_MAGIC:
            return []
        is64 = data[elf_base + 4] == 2
        little = data[elf_base + 5] == 1
        if not is64:
            return []
        end = "<" if little else ">"
        e_shoff = struct.unpack_from(end + "Q", data, elf_base + 0x28)[0]
        e_shentsize = struct.unpack_from(end + "H", data, elf_base + 0x3A)[0]
        e_shnum = struct.unpack_from(end + "H", data, elf_base + 0x3C)[0]
        e_shstrndx = struct.unpack_from(end + "H", data, elf_base + 0x3E)[0]
        if e_shoff == 0 or e_shnum == 0 or e_shentsize < 64:
            return []
        secs = []
        for i in range(e_shnum):
            base = elf_base + e_shoff + i * e_shentsize
            if base + 64 > len(data):
                break
            sh_name = struct.unpack_from(end + "I", data, base + 0)[0]
            sh_type = struct.unpack_from(end + "I", data, base + 4)[0]
            sh_flags = struct.unpack_from(end + "Q", data, base + 8)[0]
            sh_offset = struct.unpack_from(end + "Q", data, base + 24)[0]
            sh_size = struct.unpack_from(end + "Q", data, base + 32)[0]
            secs.append({
                "name_off": sh_name, "type": sh_type, "flags": sh_flags,
                "offset": elf_base + sh_offset, "size": sh_size,
                "executable": bool(sh_flags & _SHF_EXECINSTR), "name": "",
            })
        if 0 <= e_shstrndx < len(secs):
            strtab = secs[e_shstrndx]
            so, ss = strtab["offset"], strtab["size"]
            blob = data[so:so + ss] if so + ss <= len(data) else b""
            for s in secs:
                no = s["name_off"]
                if 0 <= no < len(blob):
                    z = blob.find(b"\x00", no)
                    s["name"] = blob[no:z].decode("latin1", "replace")
        return secs
    except Exception:
        return []


def _flag_names(flags: int):
    out = []
    if flags & _SHF_ALLOC:
        out.append("ALLOC")
    if flags & _SHF_WRITE:
        out.append("WRITE")
    if flags & _SHF_EXECINSTR:
        out.append("EXECINSTR")
    return out


def _section_for_offset(secs, off):
    for s in secs:
        if s["type"] == _SHT_NOBITS:
            continue
        if s["size"] and s["offset"] <= off < s["offset"] + s["size"]:
            return s
    return None


# ---- classification --------------------------------------------------------

def classify_file(data: bytes) -> str:
    if len(data) >= 4 and data[:4] == _SELF_MAGIC:
        return FMT_SELF
    if len(data) >= 4 and data[:4] == _ELF_MAGIC:
        return FMT_ELF
    return FMT_NON_ELF


# ---- raw field search -----------------------------------------------------

def find_fields(data: bytes):
    """Yield (offset, four_bytes) for each raw '?? F1 63 2D'. Anchored on the
    3-byte tail; never matches a lone leading byte."""
    out = []
    i = 0
    while True:
        j = data.find(_FIELD_TAIL, i)
        if j == -1:
            break
        if j >= 1:
            off = j - 1
            out.append((off, bytes(data[off:off + _FIELD_LEN])))
        i = j + 1
    return out


def _state_for(leading_byte: int) -> str:
    return "unpatched" if (leading_byte & _PATCH_BIT) else "patched"


def _confidence_for(*, known, tail_ok, fmt, section):
    """Revised confidence: known-offset is the dominant signal.

    HIGH   = known offset AND tail matches.
    MEDIUM = tail matches, offset unknown, file looks like an executable
             container (ELF or SELF/fSELF).
    LOW    = tail matches in an unknown/unclassified (NON_ELF) file.
    SKIP   = (handled before this is called) unreadable only.
    Section info, when present, is informational and does NOT gate confidence.
    """
    if not tail_ok:
        return "LOW"
    if known:
        return "HIGH"
    if fmt in (FMT_ELF, FMT_SELF):
        return "MEDIUM"
    return "LOW"


# ---- per-file scan --------------------------------------------------------

def scan_file(path, *, overmatch_ceiling=DEFAULT_OVERMATCH_CEILING):
    """Scan one file (read-only, raw bytes). Returns a file-result dict.

    The ONLY skip reason is unreadable. SELF/fSELF/NON_ELF are raw-scanned.
    """
    result = {
        "file_path": path,
        "file_name": os.path.basename(path),
        "scanned": False,
        "readable": False,
        "format": FMT_UNREADABLE,
        "elf_parsed": False,
        "embedded_elf_offset": None,
        "section_count": 0,
        "candidates": [],
        "skip_reason": None,
    }
    try:
        with open(path, "rb") as f:
            data = f.read()
        result["readable"] = True
    except Exception as e:
        result["skip_reason"] = "unreadable: %s" % e
        return result

    fmt = classify_file(data)
    result["format"] = fmt
    result["scanned"] = True

    # OPTIONAL enrichment: sections only when a plaintext ELF is reachable.
    secs = []
    if fmt == FMT_ELF:
        secs = _parse_elf_sections(data, 0)
    elif fmt == FMT_SELF:
        eoff = _find_embedded_elf_offset(data)
        if eoff >= 0:
            result["embedded_elf_offset"] = eoff
            secs = _parse_elf_sections(data, eoff)
    result["elf_parsed"] = bool(secs)
    result["section_count"] = len(secs)

    # RAW whole-file scan for the full 4-byte field.
    fields = find_fields(data)
    n = len(data)
    for off, field in fields:
        tail_ok = field[1:4] == _FIELD_TAIL and len(field) == _FIELD_LEN
        lead = field[0]
        sec = _section_for_offset(secs, off) if secs else None
        known = off in KNOWN_OFFSETS
        conf = _confidence_for(known=known, tail_ok=tail_ok, fmt=fmt,
                               section=sec)
        b0 = max(0, off - _CONTEXT_BEFORE)
        a1 = min(n, off + _FIELD_LEN + _CONTEXT_AFTER)
        result["candidates"].append({
            "file_path": path,
            "file_name": os.path.basename(path),
            "offset": off,
            "offset_hex": "0x%X" % off,
            "field_bytes": field.hex(),
            "leading_byte": "0x%02X" % lead,
            "tail_ok": tail_ok,
            "bit2_set": bool(lead & _PATCH_BIT),
            "state": _state_for(lead),
            "section": {
                "name": sec["name"] or "(unnamed)",
                "type": sec["type"],
                "flags": _flag_names(sec["flags"]),
                "executable": sec["executable"],
                "resolved": True,
            } if sec else {
                "name": None, "type": None, "flags": [],
                "executable": None, "resolved": False,
            },
            "context": {
                "before": data[b0:off].hex(),
                "field": field.hex(),
                "after": data[off + _FIELD_LEN:a1].hex(),
            },
            "known_offset_match": known,
            "confidence": conf,
        })

    if len(result["candidates"]) > overmatch_ceiling:
        # Too many hits: the value is too common to target safely.
        result["overmatch"] = True
        result["overmatch_count"] = len(result["candidates"])
    return result


def enumerate_targets(dump_folder, *, whole_folder=False):
    found = []
    seen = set()

    def _add(p):
        ap = os.path.abspath(p)
        if ap not in seen and os.path.isfile(ap):
            seen.add(ap)
            found.append(ap)

    for name in _DEFAULT_TARGETS:
        _add(os.path.join(dump_folder, name))
    for d in _DEFAULT_DIRS:
        dd = os.path.join(dump_folder, d)
        if os.path.isdir(dd):
            for root, _dirs, files in os.walk(dd):
                for fn in files:
                    _add(os.path.join(root, fn))
    if whole_folder:
        for root, _dirs, files in os.walk(dump_folder):
            for fn in files:
                if fn.lower().endswith(_WHOLE_FOLDER_EXTS):
                    _add(os.path.join(root, fn))
    return found


def validate_profile_relative(profile_offsets, found_offsets,
                              near_tol=PROFILE_NEAR_TOL):
    """Determine whether `found_offsets` are the `profile_offsets` structure
    relocated by a constant delta. Pure arithmetic, read-only.

    Tries every (profile, found) pairing as the anchor and picks the delta that
    explains the most profile offsets (exact, then near). Returns a dict:
        {anchor_profile, anchor_found, delta, exact, near, missing, details,
         verdict}
    or None if there is nothing to compare.

    verdict is one of:
        'EXACT_RELOCATION'   — all profile offsets relocate exactly
        'STRUCTURAL_MATCH'   — all explained, >=1 exact, some near
        'PARTIAL_STRUCTURAL' — >=2 explained but not all
        'NO_RELOCATION'      — profile does not fit this build
    """
    if not profile_offsets or not found_offsets:
        return None
    found_set = set(found_offsets)
    best = None
    for anchor_p in profile_offsets:
        for anchor_f in found_offsets:
            delta = anchor_f - anchor_p
            exact = near = missing = 0
            details = []
            for p in profile_offsets:
                pred = p + delta
                if pred in found_set:
                    exact += 1
                    details.append({"profile_offset": p, "predicted": pred,
                                    "matched": pred, "kind": "exact",
                                    "delta_off": 0})
                else:
                    nearest = min(found_offsets, key=lambda f: abs(f - pred))
                    off = nearest - pred
                    if abs(off) <= near_tol:
                        near += 1
                        details.append({"profile_offset": p, "predicted": pred,
                                        "matched": nearest, "kind": "near",
                                        "delta_off": off})
                    else:
                        missing += 1
                        details.append({"profile_offset": p, "predicted": pred,
                                        "matched": None, "kind": "missing",
                                        "delta_off": None})
            score = (exact, near, -abs(delta))
            if best is None or score > best["_score"]:
                best = {
                    "anchor_profile": anchor_p, "anchor_found": anchor_f,
                    "delta": delta, "exact": exact, "near": near,
                    "missing": missing, "details": details, "_score": score,
                }
    if best is None:
        return None
    total = len(profile_offsets)
    if best["exact"] == total:
        verdict = "EXACT_RELOCATION"
    elif best["exact"] + best["near"] == total and best["exact"] >= 1:
        verdict = "STRUCTURAL_MATCH"
    elif best["exact"] + best["near"] >= 2:
        verdict = "PARTIAL_STRUCTURAL"
    else:
        verdict = "NO_RELOCATION"
    best["verdict"] = verdict
    del best["_score"]
    return best


def _apply_structural_confidence(scan_result):
    """If a profile relocates onto the found candidates (STRUCTURAL_MATCH or
    better), upgrade the confidence of the candidates that sit at the matched
    (exact/near) predicted offsets from MEDIUM/LOW to STRUCTURAL. Read-only;
    mutates only the in-memory result. Never upgrades to HIGH (HIGH is reserved
    for an exact ABSOLUTE known-offset match for this build) and never implies
    a patch recommendation.

    Returns the validation dict (per best profile) or None.
    """
    all_offsets = sorted({c["offset"]
                          for fr in scan_result["files"]
                          for c in fr["candidates"]})
    if not all_offsets:
        return None
    best_val = None
    best_name = None
    for pid, prof in PROFILES.items():
        val = validate_profile_relative(prof["offsets"], all_offsets)
        if not val:
            continue
        if best_val is None or (val["exact"], val["near"]) > (
                best_val["exact"], best_val["near"]):
            best_val = val
            best_name = prof["name"]
    if not best_val:
        return None
    best_val["profile_name"] = best_name
    # Only upgrade when the whole structure is explained.
    if best_val["verdict"] not in ("EXACT_RELOCATION", "STRUCTURAL_MATCH"):
        return best_val
    matched_offsets = {d["matched"] for d in best_val["details"]
                       if d["kind"] in ("exact", "near") and d["matched"] is not None}
    for fr in scan_result["files"]:
        for c in fr["candidates"]:
            if c["offset"] in matched_offsets:
                if c["confidence"] in (CONF_MEDIUM, CONF_LOW):
                    c["confidence"] = CONF_STRUCTURAL
                    c["structural_match"] = True
    return best_val


def scan_dump(dump_folder, *, whole_folder=False,
              overmatch_ceiling=DEFAULT_OVERMATCH_CEILING):
    targets = enumerate_targets(dump_folder, whole_folder=whole_folder)
    files = [scan_file(p, overmatch_ceiling=overmatch_ceiling)
             for p in targets]

    all_c = [c for fr in files for c in fr["candidates"]]
    # Skipped = unreadable ONLY.
    skipped = [fr for fr in files if fr["skip_reason"]]
    overmatch_files = [fr for fr in files if fr.get("overmatch")]

    # Profile-relative (structural) validation: can a known profile be
    # relocated onto this build's candidates? Upgrades MEDIUM/LOW candidates
    # at matched offsets to STRUCTURAL. Never HIGH; never a patch
    # recommendation.
    validation = _apply_structural_confidence({"files": files})

    by_conf = {"HIGH": 0, "STRUCTURAL": 0, "MEDIUM": 0, "LOW": 0, "SKIP": 0}
    for c in all_c:
        by_conf[c["confidence"]] = by_conf.get(c["confidence"], 0) + 1

    matched_known = sorted({c["offset"] for c in all_c
                            if c["known_offset_match"]})
    patched = sum(1 for c in all_c if c["state"] == "patched")
    unpatched = sum(1 for c in all_c if c["state"] == "unpatched")

    # files actually scanned (raw) = readable files
    raw_scanned = sum(1 for fr in files if fr["scanned"])

    warnings = []
    for fr in overmatch_files:
        warnings.append("%s: over-match — %d candidate fields (expected <= %d); "
                        "value too common to target safely"
                        % (fr["file_name"], fr.get("overmatch_count", 0),
                           overmatch_ceiling))

    # LEGACY tier: this scan is report-only and NEVER recommends patching.
    # The historical 12->8 field pattern (0C F1 63 2D) did NOT fix audio on
    # console (DBS) and was absent from the DBS real working diff. Real fixes
    # (DBS/SM2) require channel-config / API-context changes, which the
    # channel-config scan handles. HIGH/STRUCTURAL here therefore mean
    # "legacy marker present", not "fix available".
    recommend = False
    legacy_present = by_conf["HIGH"] > 0 or by_conf.get("STRUCTURAL", 0) > 0

    # Profile-relative validation block (formatted for the summary/report)
    val_block = None
    if validation:
        val_block = {
            "profile_name": validation.get("profile_name"),
            "verdict": validation.get("verdict"),
            "anchor_profile": "0x%X" % validation["anchor_profile"],
            "anchor_found": "0x%X" % validation["anchor_found"],
            "delta": ("+0x%X" % validation["delta"]) if validation["delta"] >= 0
            else ("-0x%X" % -validation["delta"]),
            "exact": validation["exact"],
            "near": validation["near"],
            "missing": validation["missing"],
            "predictions": [
                {
                    "profile_offset": "0x%X" % d["profile_offset"],
                    "predicted": "0x%X" % d["predicted"],
                    "matched": ("0x%X" % d["matched"]) if d["matched"] is not None
                    else None,
                    "kind": d["kind"],
                    "delta_off": d["delta_off"],
                }
                for d in validation["details"]],
            # structural evidence only — NEVER a patch recommendation
            "patch_recommendation": "NO — legacy marker, not a confirmed fix",
        }

    summary = {
        "dump_folder": dump_folder,
        "whole_folder": whole_folder,
        "files_scanned": raw_scanned,
        "files_skipped": len(skipped),
        "files_raw_scanned": raw_scanned,
        "candidates_found": len(all_c),
        "known_offsets_matched": len(matched_known),
        "known_offsets_total": len(KNOWN_OFFSETS),
        "known_offsets_matched_list": ["0x%X" % o for o in matched_known],
        "already_patched": patched,
        "unpatched": unpatched,
        "by_confidence": by_conf,
        "profile_relative_validation": val_block,
        "warnings": warnings,
        "recommend_patching": recommend,
        "tier": "LEGACY_F1_FIELD",
        "legacy_marker_present": legacy_present,
        "disclaimer": (
            "LEGACY field scan (report-only). Historical SDK-style 12→8 fields "
            "(0C F1 63 2D). These are NOT a confirmed audio fix: patching exactly "
            "this pattern did NOT fix audio on console (DBS), and these fields "
            "were absent from the DBS real working diff. DBS/SM2 evidence shows "
            "real fixes require channel-config / API-context changes — run the "
            "channel-config scan. This scan never recommends patching."),
    }
    return {"files": files, "summary": summary}


def export_json(scan_result) -> str:
    return json.dumps(scan_result, indent=2, sort_keys=False)


def export_txt(scan_result) -> str:
    s = scan_result["summary"]
    lines = []
    lines.append("LEGACY field scan (0C F1 63 2D) — READ ONLY")
    lines.append("=" * 60)
    lines.append("Historical SDK-style 12→8 fields. These are a LEGACY")
    lines.append("STRUCTURAL MARKER, NOT a confirmed audio fix:")
    lines.append("  - patching exactly this pattern did NOT fix audio on")
    lines.append("    console (DBS), and these fields were ABSENT from the DBS")
    lines.append("    real working diff.")
    lines.append("  - real fixes (DBS/SM2) require channel-config / API-context")
    lines.append("    changes — run the channel-config scan.")
    lines.append("This scan is read-only and NEVER recommends patching.")
    lines.append("")
    lines.append("Dump folder:           %s" % s["dump_folder"])
    lines.append("Files raw-scanned:     %d" % s["files_raw_scanned"])
    lines.append("Files skipped (unreadable): %d" % s["files_skipped"])
    lines.append("Candidate fields:      %d" % s["candidates_found"])
    lines.append("Known offsets matched (absolute): %d / %d  %s"
                 % (s["known_offsets_matched"], s["known_offsets_total"],
                    ", ".join(s["known_offsets_matched_list"])))
    _val = s.get("profile_relative_validation")
    if _val:
        _pm = _val["exact"] + _val["near"]
        _pt = _pm + _val["missing"]
        lines.append("Profile-relative matches: %d / %d   verdict %s (delta %s)"
                     % (_pm, _pt, _val["verdict"], _val["delta"]))
    else:
        lines.append("Profile-relative matches: — (no profile relocation found)")

    lines.append("Unpatched / Patched:   %d / %d"
                 % (s["unpatched"], s["already_patched"]))
    lines.append("Confidence:            HIGH=%d STRUCTURAL=%d MEDIUM=%d LOW=%d SKIP=%d"
                 % (s["by_confidence"].get("HIGH", 0),
                    s["by_confidence"].get("STRUCTURAL", 0),
                    s["by_confidence"].get("MEDIUM", 0),
                    s["by_confidence"].get("LOW", 0),
                    s["by_confidence"].get("SKIP", 0)))
    lines.append("Tier: LEGACY_F1_FIELD (report-only)")
    lines.append("Patch recommendation: NO — legacy marker, not a confirmed fix")

    # Profile-relative validation (structural evidence; never a patch
    # recommendation)
    val = s.get("profile_relative_validation")
    if val:
        lines.append("")
        lines.append("Profile-relative validation:")
        lines.append("  Known profile: %s" % (val.get("profile_name") or "?"))
        lines.append("  Anchor:")
        lines.append("    known "
                     f"{val['anchor_profile']!s} -> found {val['anchor_found']!s}")
        lines.append("    delta %s" % val["delta"])
        lines.append("  Predicted:")
        for p in val["predictions"]:
            if p["kind"] == "exact":
                lines.append("    "
                             f"{p['profile_offset']!s} + delta = {p['predicted']!s}  FOUND exact")
            elif p["kind"] == "near":
                lines.append("    %s + delta = %s  NEAR found at %s (%s%d)"
                             % (p["profile_offset"], p["predicted"],
                                p["matched"],
                                "+" if (p["delta_off"] or 0) >= 0 else "",
                                p["delta_off"]))
            else:
                lines.append("    "
                             f"{p['profile_offset']!s} + delta = {p['predicted']!s}  MISSING")
        lines.append("  Conclusion: %s" % val["verdict"])
        lines.append("  Patch recommendation: %s" % val["patch_recommendation"])
    if s["warnings"]:
        lines.append("")
        lines.append("WARNINGS:")
        for w in s["warnings"]:
            lines.append("  ! " + w)
    lines.append("")
    lines.append("Candidate audio-fix data fields:")
    lines.append("-" * 60)
    any_row = False
    for fr in scan_result["files"]:
        if fr["skip_reason"]:
            lines.append("  [SKIP] "
                         f"{fr['file_name']!s} — {fr['skip_reason']!s}")
            continue
        if not fr["candidates"]:
            lines.append("  [----] "
                         f"{fr['file_name']!s} — raw-scanned ({fr['format']!s}), no candidates")
            continue
        for c in fr["candidates"]:
            any_row = True
            sec = c["section"]
            sec_str = sec["name"] if sec["resolved"] else "not resolved"
            flags = "|".join(sec["flags"]) if sec["flags"] else "-"
            lines.append("  ["
                         f"{c['confidence']!s}] {c['file_name']!s} @ {c['offset_hex']!s}  "
                         f"{' '.join(c['field_bytes'][i:i + 2].upper() for i in range(0, 8, 2))!s}  "
                         f"{c['state']!s}  fmt={fr['format']!s} sec={sec_str!s} flags={flags!s} lead="
                         f"{c['leading_byte']!s}"
                         f"{'  [KNOWN]' if c['known_offset_match'] else ''!s}")
    if not any_row and s["candidates_found"] == 0:
        lines.append("  (raw scanned — no candidate fields found)")
    return "\n".join(lines)
