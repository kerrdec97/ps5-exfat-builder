"""
ui/audio_profiles.py  —  Confirmed audio-fix profile library.

A profile is a console-confirmed channel-config audio fix for ONE title/build,
derived from a clean broken->working eboot diff plus a console result. Only
profiles in this library are PATCH-ELIGIBLE. Everything the detector finds
heuristically (twin-branch matches, standalone immediates, loose clusters) is
REPORT-ONLY unless it matches a confirmed profile here.

Hard rule enforced by the caller: detection != patch authorization.

Channel-count ENCODINGS (the value 12 -> 8 appears several ways across titles):
  DIRECT_IMM8     push 12 / mov [mem], 12         value byte == 0x0C
  IMM32_LOW_BYTE  mov [mem], 0xXXXXXX0C           low byte  == 0x0C
  IMM_SHIFTED_8   mov reg, 0x0C00  (== 12 << 8)   high byte == 0x0C

Structure TYPES:
  TWIN_BRANCH        two structurally-identical paths, one sets 8, one sets 12;
                     patch the 12-branch only. (DBS, SM2 — the reliable signal.)
  STANDALONE_IMM     a single immediate store with the channel value; ambiguous
                     on its own (the old DBS 'F1 63 2D' was this, and it FAILED
                     on console). Recorded for completeness, lower trust.

Each profile records, per site: the exact broken-byte signature, the patched
form, the file offset (for THAT build), the encoding, and the structure type.
Verified ground truth:
  * DBSparklingZero PPSA15210 02.000.013 — twin-branch (push/mov), 0x529279A.
  * SpiderMan2     PPSA03016 01.004.003 — twin-branch (mov eax,0xC00) at
    0x942493 + standalone imm32 (low byte) at 0x9422B5.
"""

from __future__ import annotations

# ---- encodings -------------------------------------------------------------
ENC_DIRECT_IMM8 = "DIRECT_IMM8"
ENC_IMM32_LOW_BYTE = "IMM32_LOW_BYTE"
ENC_IMM_SHIFTED_8 = "IMM_SHIFTED_8"

# ---- structure types -------------------------------------------------------
STRUCT_TWIN_BRANCH = "TWIN_BRANCH"
STRUCT_STANDALONE_IMM = "STANDALONE_IMM"

# ---- channel values --------------------------------------------------------
CH_UNPATCHED = 12
CH_PATCHED = 8


def _site(offset, broken_hex, patched_hex, encoding, structure, role):
    return {
        "offset": offset,
        "broken": bytes.fromhex(broken_hex),
        "patched": bytes.fromhex(patched_hex),
        "encoding": encoding,
        "structure": structure,
        "role": role,
    }


PROFILES = {
    "DBSparklingZero_PPSA15210_02.000.013": {
        "title": "DBSparklingZero",
        "title_id": "PPSA15210",
        "app_ver": "02.000.013",
        "provenance": "broken+working eboot diff + console-confirmed",
        "fix_summary": "channel 12->8, twin-branch (push/mov cluster)",
        # One contiguous 16-byte cluster: mov [rbp-0x6C],12 / mov [rbx],12 /
        # push 12 ; pop rax.  All three 0x0C bytes flip to 0x08.
        "sites": [
            _site(
                0x5292796,
                "c74594000c0000c7030c0000006a0c58",
                "c7459400080000c703080000006a0858",
                ENC_DIRECT_IMM8, STRUCT_TWIN_BRANCH,
                "twin-branch 12-cluster (mov/mov/push) -> 8",
            ),
        ],
        # The 8-branch twin lives just before; never touch it.
        "do_not_touch": [0x5292784],
    },
    "SpiderMan2_PPSA03016_01.004.003": {
        "title": "SpiderMan2",
        "title_id": "PPSA03016",
        "app_ver": "01.004.003",
        "provenance": "broken+working eboot diff + console-confirmed",
        "fix_summary": "channel 12->8: twin-branch (mov eax,0xC00) + standalone imm32 low-byte",
        "sites": [
            # mov eax, 0xC00  (12 << 8) -> mov eax, 0x800
            _site(
                0x942493,
                "b8000c0000",
                "b800080000",
                ENC_IMM_SHIFTED_8, STRUCT_TWIN_BRANCH,
                "twin-branch mov eax,0xC00 (12<<8) -> 0x800",
            ),
            _site(
                0x9422B5,
                "0cf1632d",
                "08f1632d",
                ENC_IMM32_LOW_BYTE, STRUCT_STANDALONE_IMM,
                "standalone imm32 low-byte 0x..0C -> 0x..08",
            ),
        ],
        # The 8-branch twin (mov eax, 0x800) — never touch it.
        "do_not_touch": [0x94248C],
    },
}


def list_profiles():
    return list(PROFILES.keys())


def get_profile(pid):
    return PROFILES.get(pid)


def match_profiles(data):
    """Return profiles whose EXACT site signatures are all present in `data`.

    A profile matches when every one of its sites' broken-or-patched signature
    is found at the recorded offset (we accept either broken or patched bytes,
    and report which). This is exact, console-confirmed matching — the only
    patch-eligible result.

    Returns a list of dicts: {profile_id, title, sites:[{offset, state,...}],
    all_present, any_unpatched}.
    """
    out = []
    for pid, prof in PROFILES.items():
        site_states = []
        all_present = True
        any_unpatched = False
        for s in prof["sites"]:
            off = s["offset"]
            blen = len(s["broken"])
            chunk = data[off:off + blen] if off + blen <= len(data) else b""
            if chunk == s["broken"]:
                state = "unpatched"
                any_unpatched = True
            elif chunk == s["patched"]:
                state = "patched"
            else:
                state = "absent"
                all_present = False
            site_states.append({
                "offset": "0x%X" % off,
                "state": state,
                "encoding": s["encoding"],
                "structure": s["structure"],
                "role": s["role"],
            })
        # A profile is reported if at least one site is present (so the user
        # sees partial matches), but it is only patch-eligible when ALL sites
        # are present (checked by the caller via all_present).
        present_count = sum(1 for ss in site_states if ss["state"] != "absent")
        if present_count > 0:
            out.append({
                "profile_id": pid,
                "title": prof["title"],
                "title_id": prof["title_id"],
                "app_ver": prof["app_ver"],
                "fix_summary": prof["fix_summary"],
                "provenance": prof["provenance"],
                "sites": site_states,
                "all_present": all_present,
                "any_unpatched": any_unpatched,
                "do_not_touch": ["0x%X" % o for o in prof.get("do_not_touch", [])],
            })
    return out
