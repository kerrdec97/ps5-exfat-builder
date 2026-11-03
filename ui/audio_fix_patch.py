"""
ui/audio_fix_patch.py  —  Audio Fix TEST PATCH engine (Phase 2, gated).

This is the FIRST write-capable piece of the audio-fix tooling. It exists so a
user can produce a *testable* artifact from STRUCTURAL evidence — WITHOUT the
tool ever implying that evidence is a confirmed fix.

Non-negotiable safety model (proven by tests, not just asserted):
  * THE ORIGINAL FILE IS NEVER WRITTEN. There is no code path that opens the
    original for writing. Patching happens on a COPY at
    <dump_folder>/audio_testpatch/<file>. The original is opened 'rb' only.
  * A .bak of the original is written beside it (hash recorded) before the copy
    is made — belt-and-suspenders on top of an already non-destructive op.
  * The patch is bit-precise: clear bit 2 of byte 0 only (0C -> 08), leave the
    F1 63 2D tail untouched. Each site is verified BEFORE (must currently be an
    unpatched '?? F1 63 2D' field) and AFTER (exactly bit 2 cleared, other 3
    bytes byte-identical). Any deviation rolls back that site.
  * STRUCTURAL is "test patch available", never "confirmed fix". Nothing here
    can produce a HIGH/confirmed result — only a real console test does that.

This module is GUI-free and stdlib-only so the safety properties are unit
testable headlessly. The gated dialog (wording, acknowledgements, default
selection) lives in tab_backports.py and calls into here.
"""

from __future__ import annotations

import os
import shutil
import hashlib
import time

# The 4-byte field: ?? F1 63 2D ; patch clears bit 2 of byte 0 (0C -> 08)
_FIELD_TAIL = bytes([0xF1, 0x63, 0x2D])
_FIELD_LEN = 4
_PATCH_BIT = 0x04

_TESTPATCH_DIRNAME = "audio_testpatch"
_BAK_SUFFIX = ".bak"


def _sha256(path, _buf=1024 * 1024) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(_buf)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def working_copy_path(dump_folder, file_path):
    """Return the working-copy path under <dump_folder>/audio_testpatch/."""
    out_dir = os.path.join(dump_folder, _TESTPATCH_DIRNAME)
    return os.path.join(out_dir, os.path.basename(file_path))


def backup_path_for(file_path):
    """Return the .bak path beside the original."""
    return file_path + _BAK_SUFFIX


def verify_site_unpatched(data: bytes, offset: int) -> tuple:
    """Confirm the 4 bytes at `offset` are an UNPATCHED '?? F1 63 2D' field.

    Returns (ok, reason). ok=False means the site must be refused (not patched).
    """
    if offset < 0 or offset + _FIELD_LEN > len(data):
        return False, "offset out of range"
    field = data[offset:offset + _FIELD_LEN]
    if field[1:4] != _FIELD_TAIL:
        return False, "tail mismatch (expected F1 63 2D, got %s)" % \
            field[1:4].hex()
    if not (field[0] & _PATCH_BIT):
        return False, "already patched (bit 2 already clear)"
    return True, "unpatched field present"


def _apply_bit_clear(data: bytearray, offset: int):
    """Clear bit 2 of byte 0 at offset. Mutates the bytearray in place."""
    data[offset] = data[offset] & ~_PATCH_BIT


def verify_site_patched(orig_field: bytes, new_field: bytes) -> tuple:
    """Confirm exactly bit 2 of byte 0 cleared and the other 3 bytes identical.

    Returns (ok, reason).
    """
    if len(new_field) != _FIELD_LEN or len(orig_field) != _FIELD_LEN:
        return False, "field length error"
    # tail must be byte-identical
    if new_field[1:4] != orig_field[1:4]:
        return False, "tail changed (must stay F1 63 2D)"
    # byte 0: exactly bit 2 cleared, nothing else
    diff = orig_field[0] ^ new_field[0]
    if diff != _PATCH_BIT:
        return False, "byte0 changed by more than bit 2 (xor=0x%02X)" % diff
    if new_field[0] & _PATCH_BIT:
        return False, "bit 2 not cleared"
    return True, "exactly bit 2 cleared, tail intact"


def create_test_patch(dump_folder, file_path, offsets, *, log=None):
    """Create a patched COPY of `file_path` with the selected `offsets` patched.

    THE ORIGINAL IS NEVER WRITTEN. Steps:
      1. Write a .bak of the original beside it (hash recorded).
      2. Copy the original to <dump_folder>/audio_testpatch/<file>.
      3. Patch the COPY in memory, verifying each site before/after.
      4. Write the patched bytes to the copy only.

    Returns a result dict (see below). Never raises into the caller for
    expected failures — they come back in the dict.
    """
    def _log(m):
        if log:
            try:
                log(m)
            except Exception:
                pass

    result = {
        "source": file_path,
        "source_sha256": None,
        "backup": None,
        "backup_sha256": None,
        "copy": None,
        "copy_sha256": None,
        "requested_offsets": ["0x%X" % o for o in offsets],
        "sites": [],
        "patched_count": 0,
        "refused_count": 0,
        "original_untouched": True,
        "ok": False,
        "error": None,
        "timestamp": _now(),
        "disclaimer": ("TEST PATCH — UNCONFIRMED. Applies a historical byte "
                       "pattern that has NOT been verified to fix audio or to "
                       "boot on this title. Requires a real console test."),
    }

    try:
        if not os.path.isfile(file_path):
            result["error"] = "source file not found"
            return result

        # --- hash the original BEFORE anything else -------------------------
        orig_hash_before = _sha256(file_path)
        result["source_sha256"] = orig_hash_before

        # --- 1. backup beside the original --------------------------------
        bak = backup_path_for(file_path)
        try:
            shutil.copy2(file_path, bak)
            result["backup"] = bak
            result["backup_sha256"] = _sha256(bak)
            _log("[TESTPATCH] Backup written: %s\n" % bak)
        except Exception as e:
            result["error"] = "backup failed: %s" % e
            return result

        # --- 2. working copy ------------------------------------------------
        copy = working_copy_path(dump_folder, file_path)
        try:
            os.makedirs(os.path.dirname(copy), exist_ok=True)
            shutil.copy2(file_path, copy)
            result["copy"] = copy
            _log("[TESTPATCH] Working copy: %s\n" % copy)
        except Exception as e:
            result["error"] = "copy failed: %s" % e
            return result

        # --- 3. patch the COPY in memory, site by site ----------------------
        with open(copy, "rb") as f:
            data = bytearray(f.read())

        for off in offsets:
            rec = {"offset": "0x%X" % off, "before": None, "after": None,
                   "patched": False, "reason": None}
            ok, reason = verify_site_unpatched(bytes(data), off)
            if not ok:
                rec["reason"] = "refused: " + reason
                result["refused_count"] += 1
                result["sites"].append(rec)
                _log("[TESTPATCH] Site 0x%X refused: %s\n" % (off, reason))
                continue
            orig_field = bytes(data[off:off + _FIELD_LEN])
            rec["before"] = orig_field.hex()
            _apply_bit_clear(data, off)
            new_field = bytes(data[off:off + _FIELD_LEN])
            vok, vreason = verify_site_patched(orig_field, new_field)
            if not vok:
                # roll back this site
                data[off:off + _FIELD_LEN] = orig_field
                rec["reason"] = "rolled back: " + vreason
                result["refused_count"] += 1
                result["sites"].append(rec)
                _log("[TESTPATCH] Site 0x%X rolled back: %s\n" % (off, vreason))
                continue
            rec["after"] = new_field.hex()
            rec["patched"] = True
            rec["reason"] = "patched (bit 2 cleared)"
            result["patched_count"] += 1
            result["sites"].append(rec)
            _log("[TESTPATCH] Site 0x%X patched: %s -> %s\n"
                 % (off, orig_field.hex(), new_field.hex()))

        # --- 4. write the COPY only ----------------------------------------
        if result["patched_count"] > 0:
            with open(copy, "wb") as f:
                f.write(bytes(data))
        result["copy_sha256"] = _sha256(copy)

        # --- safety proof: original hash unchanged --------------------------
        orig_hash_after = _sha256(file_path)
        result["original_untouched"] = orig_hash_after == orig_hash_before
        if not result["original_untouched"]:
            # This should be impossible; surface it loudly.
            result["error"] = (
                "SAFETY VIOLATION: original hash changed ("
                f"{orig_hash_before[:12]!s} -> {orig_hash_after[:12]!s})")
            _log("[TESTPATCH] %s\n" % result["error"])
            return result

        result["ok"] = result["patched_count"] > 0
        if result["patched_count"] == 0:
            result["error"] = "no sites patched (all refused/already patched)"
        return result
    except Exception as e:
        result["error"] = "unexpected: %s" % e
        return result


def export_patch_report_txt(result) -> str:
    """Human-readable test-patch report."""
    lines = []
    lines.append("Audio Fix TEST PATCH report — UNCONFIRMED")
    lines.append("=" * 60)
    lines.append(result["disclaimer"])
    lines.append("")
    lines.append("Timestamp:        %s" % result["timestamp"])
    lines.append("Source (original): %s" % result["source"])
    lines.append("  sha256:         %s" % (result["source_sha256"] or "?"))
    lines.append("  ORIGINAL UNTOUCHED: %s" % (
        "YES" if result["original_untouched"] else "NO (!)"))
    lines.append("Backup (.bak):    %s" % (result["backup"] or "?"))
    lines.append("  sha256:         %s" % (result["backup_sha256"] or "?"))
    lines.append("Patched COPY:     %s" % (result["copy"] or "?"))
    lines.append("  sha256:         %s" % (result["copy_sha256"] or "?"))
    lines.append("")
    lines.append("Patched sites: %d   Refused: %d" % (
        result["patched_count"], result["refused_count"]))
    lines.append("-" * 60)
    for s in result["sites"]:
        if s["patched"]:
            lines.append("  [PATCHED] "
                         f"{s['offset']!s}  "
                         f"{' '.join(s['before'][i:i + 2].upper() for i in range(0, 8, 2))!s} -> "
                         f"{' '.join(s['after'][i:i + 2].upper() for i in range(0, 8, 2))!s}   ("
                         f"{s['reason']!s})")
        else:
            lines.append(f"  [REFUSED] {s['offset']!s}  {s['reason']!s}")
    lines.append("")
    lines.append("NOTE: This is a TEST PATCH of unconfirmed (STRUCTURAL/MEDIUM)")
    lines.append("candidates. It is NOT a confirmed audio fix. Boot-test on a")
    lines.append("real console before trusting it. Only a successful console")
    lines.append("test can promote a profile to HIGH/confirmed.")
    return "\n".join(lines)


def export_patch_report_json(result) -> str:
    import json
    return json.dumps(result, indent=2, sort_keys=False)


# ---------------------------------------------------------------------------
# EXPERIMENTAL: selector-branch test patch (IMM_SHIFTED_8 twin-branch).
#
# Patches ONLY the 12-value byte of a 'mov r32, 0xC00' selector (0x0C -> 0x08,
# i.e. 0xC00 -> 0x800). Same non-destructive architecture as above: never
# writes the original; .bak + working copy; verify before/after; roll back on
# any unexpected delta. Nothing here creates a confirmed profile.
# ---------------------------------------------------------------------------

_SEL_VAL_UNPATCHED = 0x0C
_SEL_VAL_PATCHED = 0x08


def _selector_carrier_at(data, value_offset):
    """Given the offset of the selector VALUE byte, find and validate the
    enclosing 'mov r32, imm32' carrier (IMM_SHIFTED_8: B8..BF 00 <val> 00 00,
    optional REX). Returns (ok, carrier_start, carrier_len, reason).

    The value byte must be the high byte of a (val<<8) immediate: the imm32 is
    00 <val> 00 00, so the value byte is the SECOND imm byte. Carrier =
    [opt REX] B8|.. 00 <val> 00 00, value byte at carrier_start+ (1 or 2) +1.
    """
    if value_offset < 2 or value_offset + 3 > len(data):
        return False, None, None, "value offset out of range"
    imm_start = value_offset - 1
    op_off = value_offset - 2
    op = data[op_off]
    rex_off = op_off - 1
    has_rex = rex_off >= 0 and 0x40 <= data[rex_off] <= 0x4F
    if not (0xB8 <= op <= 0xBF):
        return False, None, None, "no mov r32,imm32 opcode at expected position (got 0x%02X)" % \
            op
    imm = data[imm_start:imm_start + 4]
    if len(imm) != 4:
        return False, None, None, "truncated immediate"
    # IMM_SHIFTED_8 shape: 00 <val> 00 00
    if imm[0] != 0 or imm[2] != 0 or imm[3] != 0:
        return False, None, None, "immediate is not IMM_SHIFTED_8 shape (got %s)" % \
            imm.hex()
    carrier_start = rex_off if has_rex else op_off
    carrier_len = (value_offset + 3) - carrier_start
    return True, carrier_start, carrier_len, "valid IMM_SHIFTED_8 carrier"


def verify_selector_unpatched(data, value_offset):
    """Confirm the selector value byte is 0x0C (12) AND sits in a valid
    IMM_SHIFTED_8 carrier. Returns (ok, reason)."""
    ok, cs, cl, reason = _selector_carrier_at(data, value_offset)
    if not ok:
        return False, "carrier check failed: " + reason
    if data[value_offset] != _SEL_VAL_UNPATCHED:
        if data[value_offset] == _SEL_VAL_PATCHED:
            return False, "already patched (value byte is 0x08 / 8ch)"
        return False, "value byte is 0x%02X, expected 0x0C (12ch)" % \
            data[value_offset]
    return True, "unpatched 12ch selector branch in valid carrier"


def create_selector_test_patch(dump_folder, file_path, value_offset, *,
                               log=None):
    """Create a patched COPY with ONLY the selector's 12-value byte changed
    (0x0C -> 0x08, i.e. 0xC00 -> 0x800). EXPERIMENTAL. THE ORIGINAL IS NEVER
    WRITTEN. Same safety architecture as create_test_patch.

    Returns a result dict mirroring create_test_patch, plus 'experimental': True
    and 'kind': 'selector'.
    """
    def _log(m):
        if log:
            try:
                log(m)
            except Exception:
                pass

    result = {
        "source": file_path,
        "source_sha256": None,
        "backup": None,
        "backup_sha256": None,
        "copy": None,
        "copy_sha256": None,
        "value_offset": "0x%X" % value_offset,
        "change": "0xC00 -> 0x800 (12ch -> 8ch)",
        "sites": [],
        "patched_count": 0,
        "refused_count": 0,
        "original_untouched": True,
        "ok": False,
        "error": None,
        "experimental": True,
        "kind": "selector",
        "timestamp": _now(),
        "disclaimer": ("EXPERIMENTAL. This selector resembles the DBS/SM2 "
                       "channel selector but is NOT a confirmed profile for "
                       "this title. Patched COPY for console testing only. "
                       "Audio may remain broken; behavior may be unchanged. "
                       "This result does NOT create a confirmed profile."),
    }

    try:
        if not os.path.isfile(file_path):
            result["error"] = "source file not found"
            return result

        orig_hash_before = _sha256(file_path)
        result["source_sha256"] = orig_hash_before

        # 1. backup beside the original
        bak = backup_path_for(file_path)
        try:
            shutil.copy2(file_path, bak)
            result["backup"] = bak
            result["backup_sha256"] = _sha256(bak)
            _log("[SELECTOR] Backup written: %s\n" % bak)
        except Exception as e:
            result["error"] = "backup failed: %s" % e
            return result

        # 2. working copy
        copy = working_copy_path(dump_folder, file_path)
        try:
            os.makedirs(os.path.dirname(copy), exist_ok=True)
            shutil.copy2(file_path, copy)
            result["copy"] = copy
            _log("[SELECTOR] Working copy: %s\n" % copy)
        except Exception as e:
            result["error"] = "copy failed: %s" % e
            return result

        # 3. patch the COPY in memory
        with open(copy, "rb") as f:
            data = bytearray(f.read())

        rec = {"offset": "0x%X" % value_offset, "before": None,
               "after": None, "patched": False, "reason": None}
        ok, reason = verify_selector_unpatched(bytes(data), value_offset)
        if not ok:
            rec["reason"] = "refused: " + reason
            result["refused_count"] += 1
            result["sites"].append(rec)
            result["error"] = "selector site refused: " + reason
            _log("[SELECTOR] Refused: %s\n" % reason)
        else:
            _ok2, cs, cl, _r = _selector_carrier_at(bytes(data), value_offset)
            carrier_before = bytes(data[cs:cs + cl])
            rec["before"] = carrier_before.hex()
            data[value_offset] = _SEL_VAL_PATCHED
            carrier_after = bytes(data[cs:cs + cl])
            # verify: exactly ONE byte changed, at the value position, 0C->08
            diff_positions = [i for i in range(len(carrier_before))
                              if carrier_before[i] != carrier_after[i]]
            vb_rel = value_offset - cs
            if (diff_positions == [vb_rel]
                    and carrier_before[vb_rel] == _SEL_VAL_UNPATCHED
                    and carrier_after[vb_rel] == _SEL_VAL_PATCHED):
                rec["after"] = carrier_after.hex()
                rec["patched"] = True
                rec["reason"] = "patched 0xC00 -> 0x800 (value byte 0x0C -> 0x08)"
                # patched_count counts the single selector site
                result["patched_count"] += 1
                _log("[SELECTOR] Patched value byte 0x%X: 0C -> 08\n"
                     % value_offset)
            else:
                # roll back
                data[value_offset] = _SEL_VAL_UNPATCHED
                rec["reason"] = "rolled back: unexpected carrier delta %s" \
                    % diff_positions
                result["refused_count"] += 1
                result["error"] = "verification failed; rolled back"
            result["sites"].append(rec)

        # 4. write the COPY only
        if result["patched_count"] > 0:
            with open(copy, "wb") as f:
                f.write(bytes(data))
        result["copy_sha256"] = _sha256(copy)

        # safety proof
        orig_hash_after = _sha256(file_path)
        result["original_untouched"] = orig_hash_after == orig_hash_before
        if not result["original_untouched"]:
            result["error"] = (
                "SAFETY VIOLATION: original hash changed ("
                f"{orig_hash_before[:12]!s} -> {orig_hash_after[:12]!s})")
            _log("[SELECTOR] %s\n" % result["error"])
            return result

        result["ok"] = result["patched_count"] > 0
        return result
    except Exception as e:
        result["error"] = "unexpected: %s" % e
        return result


def export_selector_report_txt(result) -> str:
    lines = []
    lines.append("EXPERIMENTAL Selector Test Patch report")
    lines.append("=" * 60)
    lines.append(result["disclaimer"])
    lines.append("")
    lines.append("Timestamp:         %s" % result["timestamp"])
    lines.append("Source (original): %s" % result["source"])
    lines.append("  sha256:          %s" % (result["source_sha256"] or "?"))
    lines.append("  ORIGINAL UNTOUCHED: %s" % (
        "YES" if result["original_untouched"] else "NO (!)"))
    lines.append("Backup (.bak):     %s" % (result["backup"] or "?"))
    lines.append("Patched COPY:      %s" % (result["copy"] or "?"))
    lines.append("  sha256:          %s" % (result["copy_sha256"] or "?"))
    lines.append("Value offset:      %s" % result["value_offset"])
    lines.append("Change:            %s" % result["change"])
    lines.append("")
    for s in result["sites"]:
        if s["patched"]:
            lines.append("  [PATCHED] "
                         f"{s['offset']!s}  carrier {s['before']!s} -> {s['after']!s}   ({s['reason']!s})")
        else:
            lines.append(f"  [REFUSED] {s['offset']!s}  {s['reason']!s}")
    lines.append("")
    lines.append("AFTER TESTING ON LOWER-FIRMWARE CONSOLE:")
    lines.append("  Audio still broken -> selector alone is insufficient;")
    lines.append("    investigation continues.")
    lines.append("  Audio works        -> this build is confirmed; send the")
    lines.append("    working/broken pair or result to add a CONFIRMED_PROFILE.")
    lines.append("  (This test does NOT by itself create a confirmed profile.)")
    return "\n".join(lines)


def export_selector_report_json(result) -> str:
    import json
    return json.dumps(result, indent=2, sort_keys=False)
