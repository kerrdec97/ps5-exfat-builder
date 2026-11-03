"""
ui/audio_channel_scan.py  —  Channel-config audio-fix scanner (READ-ONLY).

Ground-truth scanner built from a console-confirmed working/broken eboot pair
(DBSparklingZero PPSA15210 02.000.013). The audio fix for that title is a
3-byte channel-count reconfiguration, 0x0C (12) -> 0x08 (8), embedded as
instruction immediates in a tight cluster:

    UNPATCHED (12 channels):           PATCHED (8 channels):
      c7 45 94 00 0C                     c7 45 94 00 08
      c7 03 0C 00 00 00                  c7 03 08 00 00 00
      6a 0C 58                           6a 08 58

KEY FINDING: the individual fragments are NOT unique (c7 03 0C.. appears many
times; 6a 0C 58 appears several times), but the FULL CLUSTER — all three
fragments within a small window — appears exactly ONCE. So the scanner matches
the COMBINED cluster, never a lone fragment or a lone 0x0C byte (the old
mistake: ~156k false positives).

This also explains why the earlier "?? F1 63 2D" profile failed on console: the
real channel-count fields do NOT have an F1 63 2D tail. They are mov/push
immediates in a different region. The tail-anchored scanner could never find
them.

Confidence model:
  HIGH (confirmed profile)  exact cluster match for the known title/build
                            (DBSparklingZero PPSA15210 02.000.013)
  LEAD                      loose cluster: 3+ 0x0C immediates within 16 bytes
                            (investigation only, NOT a patch target)
  SKIP                      unreadable only

This scanner is READ-ONLY: no writes, no patch targets proposed by the loose
section, no promotion to HIGH beyond the exact confirmed profile. GUI-free,
stdlib-only, unit-testable.
"""

from __future__ import annotations

import os
import json

# ---------------------------------------------------------------------------
# Confirmed profile (ground truth from the DBS working/broken pair)
# ---------------------------------------------------------------------------
PROFILE_NAME = "DBSparklingZero PPSA15210 02.000.013 (channel 12→8)"

# Fragment templates with the channel byte as a placeholder. The three
# fragments form the cluster; we search for all three within a window.
#   A: c7 45 94 00 <v>        mov dword [rbp-0x6c], v   (only first 5 bytes)
#   B: c7 03 <v> 00 00 00     mov dword [rbx], v
#   C: 6a <v> 58              push v ; pop rax
_FRAG_A_PRE = bytes.fromhex("c7459400")
_FRAG_B_PRE = bytes.fromhex("c703")
_FRAG_B_SUF = bytes.fromhex("000000")
_FRAG_C_PRE = bytes.fromhex("6a")
_FRAG_C_SUF = bytes.fromhex("58")

_VAL_UNPATCHED = 0x0C   # 12 channels
_VAL_PATCHED = 0x08     # 8 channels

# All three fragments must sit within this many bytes of fragment A's start.
_CLUSTER_WINDOW = 24

# Loose-lead window: regions with >= N bytes of 0x0C within M bytes.
_LOOSE_WINDOW = 16
_LOOSE_MIN_0C = 3

# Save-data module presence note (informational; from the working reference)
_SAVEDATA_NOTE_FILE = "fakelib/libSceSaveData.native.sprx"

_ELF_MAGIC = b"\x7fELF"
_SELF_MAGIC = bytes([0x4F, 0x15, 0x3D, 0x1D])
_DEFAULT_TARGETS = ("eboot.bin",)
_WHOLE_FOLDER_EXTS = (".bin",)


def _frag_a(value):
    return _FRAG_A_PRE + bytes([value])


def _frag_b(value):
    return _FRAG_B_PRE + bytes([value]) + _FRAG_B_SUF


def _frag_c(value):
    return _FRAG_C_PRE + bytes([value]) + _FRAG_C_SUF


def _find_all(data, needle, start=0):
    out = []
    i = start
    while True:
        j = data.find(needle, i)
        if j == -1:
            break
        out.append(j)
        i = j + 1
    return out


def find_exact_cluster(data, value):
    """Find the combined cluster (frag A + B + C within window) for the given
    channel-count `value` (0x0C unpatched or 0x08 patched).

    Returns a list of dicts: {anchor, frag_a, frag_b, frag_c, value}. The
    fragments must all fall within _CLUSTER_WINDOW bytes of frag A's start.
    Anchoring on frag A (which is unique in the reference) keeps this precise.
    """
    fa, fb, fc = _frag_a(value), _frag_b(value), _frag_c(value)
    results = []
    for a in _find_all(data, fa):
        win_lo = a
        win_hi = a + _CLUSTER_WINDOW + len(fb)
        window = data[win_lo:win_hi]
        b_rel = window.find(fb)
        c_rel = window.find(fc)
        if b_rel != -1 and c_rel != -1:
            results.append({
                "anchor": a,
                "frag_a": a,
                "frag_b": win_lo + b_rel,
                "frag_c": win_lo + c_rel,
                "value": value,
            })
    return results


def find_loose_clusters(data, *, window=_LOOSE_WINDOW, min_0c=_LOOSE_MIN_0C,
                        cap=200):
    """Secondary LEAD detection: regions with >= min_0c bytes of 0x0C within a
    sliding `window`. Investigation leads only — NOT patch targets, and
    deliberately capped to avoid runaway output on noisy files.

    To stay meaningful (not ~156k lone-0C hits), we only report a region when
    at least `min_0c` 0x0C bytes cluster within `window`, and we coalesce
    overlapping hits into distinct regions.
    """
    positions = _find_all(data, bytes([_VAL_UNPATCHED]))
    if not positions:
        return []
    leads = []
    n = len(positions)
    i = 0
    while i < n and len(leads) < cap:
        # count how many 0x0C fall within [pos_i, pos_i + window)
        j = i
        while j < n and positions[j] - positions[i] < window:
            j += 1
        count = j - i
        if count >= min_0c:
            leads.append({
                "start": positions[i],
                "end": positions[j - 1] + 1,
                "count": count,
            })
            # skip past this region to coalesce
            i = j
        else:
            i += 1
    return leads


# ---------------------------------------------------------------------------
# Twin-branch detection (SM2/DBS-style: an 8-branch and a 12-branch of the
# SAME encoding within a short window). REPORT-ONLY.
# ---------------------------------------------------------------------------
CH_UNPATCHED = 12
CH_PATCHED = 8

# Carrier shapes that hold the channel value in a twin-branch. Each entry
# is (prefix_bytes, suffix_bytes, encoding). The value byte goes between.
#   push <v> ; pop rax             6a <v> 58          DIRECT_IMM8
#   mov eax, <v><<8                b8 00 <v> 00 00    IMM_SHIFTED_8
#   mov dword [rbx], <v>           c7 03 <v> 00 00 00 DIRECT_IMM8 (mem)
_TWIN_CARRIERS = [
    # (prefix, suffix, encoding)
    {"prefix": b"\x6a", "suffix": b"\x58", "enc": "DIRECT_IMM8"},
    # mov eax, imm32 with value in the high byte (val<<8): b8 00 <v> 00 00
    {"prefix": b"\xb8\x00", "suffix": b"\x00\x00", "enc": "IMM_SHIFTED_8"},
    # mov dword [rbx], imm32 (low byte is the value)
    {"prefix": b"\xc7\x03", "suffix": b"\x00\x00\x00", "enc": "DIRECT_IMM8_MEM"},
]
# An 8-carrier and a 12-carrier of the same encoding within this many bytes.
_TWIN_WINDOW = 48

# ---------------------------------------------------------------------------
# Audio API string corroboration (Level 0). Presence of the output-port
# creation API name in the binary raises confidence that a twin-branch is
# audio-related, but it does NOT prove the value feeds the API argument.
# ---------------------------------------------------------------------------
_AUDIO_API_NAMES = [
    b"sceAudioOut2PortCreate",
    b"sceAudioOut2",
    b"sceAudioOutOpen",
]


def find_audio_api_strings(data):
    """Return which audio API name strings are present (Level 0 corroboration).
    Read-only; presence only, no call-site or argument analysis."""
    found = []
    for name in _AUDIO_API_NAMES:
        off = data.find(name)
        if off != -1:
            found.append({"name": name.decode("ascii", "replace"),
                          "offset": "0x%X" % off})
    return found


def _decode_channel_values_no_capstone(region, base):
    """Extract channel-count values from the region WITHOUT capstone, by
    scanning for the mov r32, imm32 encodings the twin-branch detector uses:
      B8 <imm32>            mov eax, imm32
      B9/BA/BB/BE/BF ...    mov ecx/edx/ebx/esi/edi, imm32
      41 B8.. etc           mov r8d.., imm32 (REX.B prefix)
    Only IMM_SHIFTED_8 style values (n<<8, i.e. low byte 0, high byte = channel
    count) are reported, matching the selector shape. Returns the same
    {imm, channels} pairs the capstone path produces, so the report is
    identical with or without capstone installed.
    """
    pairs = []
    seen = set()
    n = len(region)
    i = 0
    # Walk byte-by-byte; a 5-byte mov r32,imm32 (opt. 1-byte REX) is enough.
    while i < n - 4:
        op = region[i]
        rex = False
        j = i
        if 0x40 <= op <= 0x4F and i + 1 < n and 0xB8 <= region[i + 1] <= 0xBF:
            rex = True
            j = i + 1
            op = region[j]
        if 0xB8 <= op <= 0xBF and j + 5 <= n:
            imm = int.from_bytes(region[j + 1:j + 5], "little")
            # selector shape: low byte 0, high byte small (1..64 channels)
            if imm and (imm & 0xFF) == 0 and (imm >> 8) <= 64 and \
                    (imm >> 16) == 0 and (imm >> 8) >= 1:
                ch = imm >> 8
                if ch not in seen:
                    seen.add(ch)
                    pairs.append({"imm": "0x%X" % imm, "channels": ch})
            i = j + 5
            continue
        i += 1
    pairs.sort(key=lambda p: p["channels"])
    return pairs


def _try_disassemble(region_bytes, base_addr):
    """Disassemble a region with capstone if available; else return None.
    Capstone is optional (not a bundled dependency), so this degrades
    gracefully."""
    try:
        import capstone
    except Exception:
        return None
    try:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        out = []
        for insn in md.disasm(region_bytes, base_addr):
            out.append({
                "addr": "0x%X" % insn.address,
                "bytes": insn.bytes.hex(),
                "mnemonic": insn.mnemonic,
                "op_str": insn.op_str,
            })
        return out
    except Exception:
        return None


def _decode_channel_value(encoding, value_byte):
    """Map a carrier's value byte to a human channel count given the encoding."""
    if encoding == "IMM_SHIFTED_8":
        return value_byte
    return value_byte


def twin_branch_investigation(data, twin, *, context=24):
    """Produce an investigation report for ONE detected twin-branch: the region
    bytes, decoded values, optional disassembly, and nearby audio-API string
    references. READ-ONLY. This is an INVESTIGATION AID — it never authorizes a
    patch; the API proximity is corroboration, not proof of argument linkage.
    """
    twelve = int(twin["twelve_offset"], 16)
    eight = int(twin["eight_offset"], 16)
    lo = max(0, min(twelve, eight) - context)
    hi = min(len(data), max(twelve, eight) + context)
    region = data[lo:hi]

    report = {
        "encoding": twin["encoding"],
        "region_start": "0x%X" % lo,
        "region_end": "0x%X" % hi,
        "region_hex": region.hex(),
        "eight_offset": twin["eight_offset"],
        "twelve_offset": twin["twelve_offset"],
        "gap": twin["gap"],
        "eight_value": "0x%X" % 0x800 if twin["encoding"] == "IMM_SHIFTED_8"
        else "0x8",
        "twelve_value": "0x%X" % 0xC00
        if twin["encoding"] == "IMM_SHIFTED_8" else "0xC",
        "disassembly": None,
        "disassembly_available": False,
        "channel_values": [],
        "channel_value_pairs": [],
        "nearby_api_refs": [],
        "note": ("Investigation aid only. A twin-branch is a strong lead but NOT "
                 "a patch target unless matched by a console-confirmed profile. "
                 "The API proximity below is corroboration, not proof that the "
                 "value feeds the API argument."),
    }

    # Optional disassembly (capstone), else a no-capstone decoder so the
    # channel values are shown either way.
    dis = _try_disassemble(region, lo)
    if dis is not None:
        report["disassembly"] = dis
        report["disassembly_available"] = True
        # Collect channel-count immediates from the disassembly: any mov
        # with an immediate of shape (n << 8), n in 1..64. Also record the
        # (imm, channels) pairs so the report can show "0xC00 (12ch)".
        chans = []
        chan_pairs = []
        for d in dis:
            if d["mnemonic"] == "mov" and "0x" in d["op_str"]:
                try:
                    val = int(d["op_str"].split("0x")[-1], 16)
                    if val and val % 256 == 0 and 0 < (val >> 8) <= 64:
                        ch = val >> 8
                        chans.append(ch)
                        chan_pairs.append({"imm": "0x%X" % val, "channels": ch})
                except Exception:
                    pass
        report["channel_values"] = sorted(set(chans))
        # de-dupe pairs by channel count, keep first occurrence order by value
        seen = set()
        uniq = []
        for p in sorted(chan_pairs, key=lambda x: x["channels"]):
            if p["channels"] not in seen:
                seen.add(p["channels"])
                uniq.append(p)
        report["channel_value_pairs"] = uniq
    else:
        # No capstone: decode mov r32,imm32 immediates directly from bytes.
        pairs = _decode_channel_values_no_capstone(region, lo)
        report["channel_value_pairs"] = pairs
        report["channel_values"] = sorted(p["channels"] for p in pairs)

    # Nearby audio-API string references: search a wide window around the
    # region for the API names (string proximity, NOT call linkage).
    api_window = 0x40000
    for name in _AUDIO_API_NAMES:
        i = data.rfind(name, max(0, lo - api_window), hi + api_window)
        if i != -1:
            report["nearby_api_refs"].append({
                "name": name.decode("ascii", "replace"),
                "offset": "0x%X" % i,
                "distance": abs(i - lo),
            })
    return report


def _find_carriers(data, channel_value):
    hits = []
    for c in _TWIN_CARRIERS:
        needle = c["prefix"] + bytes([channel_value]) + c["suffix"]
        i = 0
        while True:
            j = data.find(needle, i)
            if j == -1:
                break
            hits.append({"offset": j, "enc": c["enc"],
                         "value_offset": j + len(c["prefix"])})
            i = j + 1
    return hits


def find_twin_branches(data, *, window=_TWIN_WINDOW, cap=100):
    """Find twin branches: an 8-carrier and a 12-carrier of the SAME encoding
    within `window` bytes. REPORT-ONLY candidates (never patch targets here).
    """
    eights = _find_carriers(data, CH_PATCHED)
    twelves = _find_carriers(data, CH_UNPATCHED)
    results = []
    for tw in twelves:
        if len(results) >= cap:
            break
        for ei in eights:
            if ei["enc"] != tw["enc"]:
                continue
            gap = abs(ei["offset"] - tw["offset"])
            if 0 < gap <= window:
                lo = min(ei["offset"], tw["offset"])
                hi = max(ei["offset"], tw["offset"])
                # region covers both carriers plus a little context
                ctx_lo = max(0, lo - 2)
                ctx_hi = min(len(data), hi + 8)
                results.append({
                    "encoding": tw["enc"],
                    "twelve_offset": "0x%X" % tw["offset"],
                    "eight_offset": "0x%X" % ei["offset"],
                    "twelve_value_offset": "0x%X" % tw["value_offset"],
                    "gap": gap,
                    "region_start": "0x%X" % ctx_lo,
                    "region_end": "0x%X" % ctx_hi,
                    "region_hex": data[ctx_lo:ctx_hi].hex(),
                    "note": ("twin-branch (8 & 12 of same encoding in proximity) "
                             "— REPORT ONLY, not a patch target unless matched by "
                             "a confirmed profile"),
                })
                # one twin per 12-carrier is enough for a report
                break
    return results


def classify_file(data):
    if len(data) >= 4 and data[:4] == _SELF_MAGIC:
        return "SELF_OR_FSELF"
    if len(data) >= 4 and data[:4] == _ELF_MAGIC:
        return "ELF"
    return "NON_ELF"


def _context_hex(data, off, before=2, after=10):
    lo = max(0, off - before)
    hi = min(len(data), off + after)
    return data[lo:hi].hex()


def scan_file(path):
    """Scan one file (read-only) for the channel-config cluster."""
    result = {
        "file_path": path,
        "file_name": os.path.basename(path),
        "readable": False,
        "format": "UNREADABLE",
        "profile_matches": [],
        "exact_clusters": [],
        "twin_branches": [],
        "audio_api_strings": [],
        "loose_leads": [],
        "status": None,
        "tier": None,
        "skip_reason": None,
    }
    # Memory note: for large binaries (hundreds of MB) a full read() is
    # fine on a desktop, but mmap keeps the working set small and lets the
    # OS page the file in on demand. Fall back to read() when mmap is not
    # available (e.g. an empty file, or a platform/filesystem that refuses).
    # All scanning below is read-only and works on either object because
    # both support slicing, .find() and .rfind().
    _MMAP_MIN = 32 * 1024 * 1024
    try:
        with open(path, "rb") as f:
            _sz = os.fstat(f.fileno()).st_size
            data = None
            if _sz >= _MMAP_MIN:
                try:
                    import mmap as _mmap
                    data = _mmap.mmap(f.fileno(), 0,
                                      access=_mmap.ACCESS_READ)
                except (ValueError, OSError):
                    data = None
            if data is None:
                data = f.read()
        result["readable"] = True
    except Exception as e:
        result["skip_reason"] = "unreadable: %s" % e
        return result

    result["format"] = classify_file(data)

    # Confirmed-profile matching (exact signatures; the ONLY patch-eligible result)
    try:
        from ui import audio_profiles as _ap
        result["profile_matches"] = _ap.match_profiles(data)
    except Exception:
        result["profile_matches"] = []

    unpatched = find_exact_cluster(data, _VAL_UNPATCHED)
    patched = find_exact_cluster(data, _VAL_PATCHED)

    for c in unpatched:
        result["exact_clusters"].append({
            "state": "unpatched",
            "channel_value": 12,
            "anchor_hex": "0x%X" % c["anchor"],
            "sites": [
                {"offset": "0x%X" % (c["frag_a"] + len(_FRAG_A_PRE)),
                 "role": "mov [rbp-0x6c], 12", "value": "0x0C"},
                {"offset": "0x%X" % (c["frag_b"] + len(_FRAG_B_PRE)),
                 "role": "mov [rbx], 12", "value": "0x0C"},
                {"offset": "0x%X" % (c["frag_c"] + len(_FRAG_C_PRE)),
                 "role": "push 12", "value": "0x0C"},
            ],
            "context": _context_hex(data, c["anchor"], 1, 20),
        })
    for c in patched:
        result["exact_clusters"].append({
            "state": "patched",
            "channel_value": 8,
            "anchor_hex": "0x%X" % c["anchor"],
            "sites": [
                {"offset": "0x%X" % (c["frag_a"] + len(_FRAG_A_PRE)),
                 "role": "mov [rbp-0x6c], 8", "value": "0x08"},
                {"offset": "0x%X" % (c["frag_b"] + len(_FRAG_B_PRE)),
                 "role": "mov [rbx], 8", "value": "0x08"},
                {"offset": "0x%X" % (c["frag_c"] + len(_FRAG_C_PRE)),
                 "role": "push 8", "value": "0x08"},
            ],
            "context": _context_hex(data, c["anchor"], 1, 20),
        })

    if unpatched and not patched:
        result["status"] = "PRESENT_UNPATCHED"
    elif patched and not unpatched:
        result["status"] = "ALREADY_PATCHED"
    elif unpatched and patched:
        result["status"] = "MIXED"
    else:
        result["status"] = "MISSING"

    # Twin-branch detection (REPORT-ONLY)
    result["twin_branches"] = find_twin_branches(data)
    # Attach an investigation report (region bytes, decoded values, optional
    # disassembly, nearby API refs) to each twin-branch. READ-ONLY.
    for tb in result["twin_branches"]:
        tb["investigation"] = twin_branch_investigation(data, tb)

    # Level 0 corroboration: audio API name strings present?
    result["audio_api_strings"] = find_audio_api_strings(data)

    # Loose leads (investigation only)
    for lead in find_loose_clusters(data):
        result["loose_leads"].append({
            "start": "0x%X" % lead["start"],
            "end": "0x%X" % lead["end"],
            "count_0c": lead["count"],
            "note": "investigation lead only — NOT a patch target",
        })

    # Tier: CONFIRMED_PROFILE > TWIN_BRANCH_MATCH > WEAK_LEAD > NO_MATCH
    if result["profile_matches"]:
        result["tier"] = "CONFIRMED_PROFILE"
    elif result["twin_branches"]:
        result["tier"] = "TWIN_BRANCH_MATCH"
    elif result["loose_leads"]:
        result["tier"] = "WEAK_LEAD"
    else:
        result["tier"] = "NO_MATCH"
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
    if whole_folder:
        for root, _dirs, files in os.walk(dump_folder):
            for fn in files:
                if fn.lower().endswith(_WHOLE_FOLDER_EXTS):
                    _add(os.path.join(root, fn))
    return found


def _has_savedata_module(dump_folder):
    p = os.path.join(dump_folder, _SAVEDATA_NOTE_FILE.replace("/", os.sep))
    return os.path.isfile(p)


def scan_dump(dump_folder, *, whole_folder=False):
    targets = enumerate_targets(dump_folder, whole_folder=whole_folder)
    files = [scan_file(p) for p in targets]

    exact_total = sum(len(fr["exact_clusters"]) for fr in files)
    unpatched_clusters = sum(
        1 for fr in files for c in fr["exact_clusters"]
        if c["state"] == "unpatched")
    patched_clusters = sum(
        1 for fr in files for c in fr["exact_clusters"]
        if c["state"] == "patched")
    leads_total = sum(len(fr["loose_leads"]) for fr in files)
    twin_total = sum(len(fr["twin_branches"]) for fr in files)
    profile_matches = [m for fr in files for m in fr["profile_matches"]]

    # Tier / confidence:
    #   CONFIRMED_PROFILE  — exact console-confirmed signatures present (HIGH)
    #   TWIN_BRANCH_MATCH  — 8/12 twin of same encoding found (report-only)
    #   WEAK_LEAD          — loose 0x0C clusters only (report-only)
    #   NO_MATCH
    if profile_matches:
        tier = "CONFIRMED_PROFILE"
        confidence = "HIGH"
    elif twin_total > 0:
        tier = "TWIN_BRANCH_MATCH"
        confidence = "TWIN_BRANCH"
    elif leads_total > 0:
        tier = "WEAK_LEAD"
        confidence = "NONE"
    else:
        tier = "NO_MATCH"
        confidence = "NONE"

    # Patch eligibility is STRICTLY a confirmed profile with unpatched sites.
    patch_eligible = any(m["any_unpatched"] for m in profile_matches)

    savedata_present = _has_savedata_module(dump_folder)

    summary = {
        "dump_folder": dump_folder,
        "profile_name": PROFILE_NAME,
        "files_scanned": sum(1 for fr in files if fr["readable"]),
        "files_skipped": sum(1 for fr in files if fr["skip_reason"]),
        "tier": tier,
        "confirmed_profile_matches": [
            {"profile_id": m["profile_id"], "title": m["title"],
             "app_ver": m["app_ver"], "any_unpatched": m["any_unpatched"],
             "all_present": m["all_present"], "sites": m["sites"],
             "fix_summary": m["fix_summary"], "do_not_touch": m["do_not_touch"]}
            for m in profile_matches],
        "patch_eligible": patch_eligible,
        "exact_clusters_found": exact_total,
        "unpatched_clusters": unpatched_clusters,
        "patched_clusters": patched_clusters,
        "twin_branches_found": twin_total,
        "loose_leads_found": leads_total,
        "confidence": confidence,
        "savedata_module_present": savedata_present,
        "savedata_note": (
            "Working reference also includes %s. This is present in the working "
            "release, but not proven to be the audio fix. The confirmed eboot "
            "audio-config change is the 3-byte 12→8 cluster."
            % _SAVEDATA_NOTE_FILE),
        "disclaimer": (
            "Channel-config audio-fix scan. Read-only. Detection is NOT patch "
            "authorization: only a CONFIRMED_PROFILE (console-backed) is "
            "patch-eligible. Twin-branch matches and loose clusters are "
            "REPORT-ONLY. Offsets are per-title/build, NOT universal."),
    }
    return {"files": files, "summary": summary}


def export_json(scan_result):
    return json.dumps(scan_result, indent=2, sort_keys=False)


def export_txt(scan_result):
    s = scan_result["summary"]
    lines = []
    lines.append("Channel-config audio-fix scan — READ ONLY")
    lines.append("=" * 62)
    lines.append("Tier: "
                 f"{s.get('tier', 'NO_MATCH')!s}   Confidence: {s['confidence']!s}")
    lines.append("Patch eligible: %s" % (
        "YES (confirmed profile, unpatched)" if
        s.get("patch_eligible") else "NO"))
    lines.append("")
    lines.append("Files scanned: %d   Skipped (unreadable): %d"
                 % (s["files_scanned"], s["files_skipped"]))
    lines.append("Confirmed-profile matches: %d   Twin-branch matches: %d   Loose leads: %d"
                 % (len(s.get("confirmed_profile_matches", [])),
                    s.get("twin_branches_found", 0),
                    s["loose_leads_found"]))
    lines.append("")

    # ---- PRIMARY INVESTIGATION TARGET (when no confirmed profile) --------
    # Surface the strongest lead at the top so the user does not have to dig
    # through per-file sections. Still REPORT-ONLY.
    if not s.get("confirmed_profile_matches") and s.get("twin_branches_found"):
        primary = None
        for fr in scan_result["files"]:
            if fr.get("twin_branches"):
                primary = (fr["file_name"], fr["twin_branches"][0])
                break
        if primary:
            fname, tb = primary
            inv = tb.get("investigation") or {}
            pairs = inv.get("channel_value_pairs") or []
            chans = inv.get("channel_values") or []
            lines.append("PRIMARY INVESTIGATION TARGET")
            lines.append("-" * 62)
            lines.append("  File:      %s" % fname)
            lines.append("  Region:    "
                         f"{inv.get('region_start', tb.get('region_start', '?'))!s}.."
                         f"{inv.get('region_end', tb.get('region_end', '?'))!s}")
            lines.append("  Encoding:  %s" % tb["encoding"])
            lines.append("  8-branch:  "
                         f"{tb['eight_offset']!s}    12-branch: {tb['twelve_offset']!s}")
            if pairs:
                lines.append("  Values (channel-count selector):")
                for p in pairs:
                    lines.append("    %-6s (%dch)" % (p["imm"], p["channels"]))
            elif chans:
                lines.append("  Values:    %s (channel-count selector)"
                             % " / ".join(str(c) for c in chans))
            lines.append("  Tier:      TWIN_BRANCH_MATCH")
            lines.append("  Patch eligible: NO")
            lines.append("  Reason:    no confirmed profile for this title/build")
            # Interpretation when an 8/12 pair is present: likely a firmware
            # channel-count selector (newer FW = 12ch path, older FW = 8ch).
            ch_set = set(c for c in (chans or [p["channels"] for p in pairs]))
            if 8 in ch_set and 12 in ch_set:
                lines.append("  Interpretation: a 1/2/8/12-style channel-count "
                             "selector. Newer-firmware paths may use 12-channel "
                             "audio config; lower firmware may support only 8. "
                             "For lower firmware, the 12-channel path (→ the "
                             "0xC00 / 12 branch) MAY need forcing to 8 (0x800). "
                             "This is a likely firmware/channel compatibility "
                             "shape — NOT confirmed for this title.")
                lines.append("  Suggested console test (copy only): patch "
                             "0xC00 → 0x800 at the 12-branch (value byte %s), "
                             "test on lower-firmware console. If audio works, "
                             "this title becomes a confirmed profile."
                             % tb.get("twelve_value_offset",
                                      tb["twelve_offset"]))
            if inv.get("nearby_api_refs"):
                lines.append("  Nearby audio API: %s (corroboration, not proof)"
                             % ", ".join(r["name"]
                                         for r in inv["nearby_api_refs"]))
            else:
                lines.append("  Nearby audio API: none in window (proximity is not a reliable linkage signal anyway)")
            lines.append("  -> This is the strongest lead in the scan. Inspect the "
                         "disassembly below. NOT a patch target until a "
                         "console-confirmed profile exists for this title.")
            lines.append("")

    # ---- Confirmed profile section --------------------------------------
    cpm = s.get("confirmed_profile_matches", [])
    if cpm:
        lines.append("CONFIRMED PROFILE MATCH (console-backed, patch-eligible):")
        for m in cpm:
            state = "UNPATCHED" if m["any_unpatched"] else \
                "already patched"
            lines.append("  "
                         f"{m['profile_id']!s}  ({m['title']!s} {m['app_ver']!s})  — {state!s}")
            lines.append("    fix: %s" % m["fix_summary"])
            for site in m["sites"]:
                lines.append("      "
                             f"{site['offset']!s} : {site['state']!s}  ["
                             f"{site['structure']!s} / {site['encoding']!s}]  "
                             f"{site['role']!s}")
            if m["do_not_touch"]:
                lines.append("      do NOT touch (matching 8-branch): %s"
                             % ", ".join(m["do_not_touch"]))
        lines.append("")
    else:
        lines.append("CONFIRMED PROFILE MATCH: none (no console-backed profile for this title/build)")
        # Tier/eligibility already stated above.
        lines.append("")

    for fr in scan_result["files"]:
        if fr["skip_reason"]:
            lines.append("[SKIP] "
                         f"{fr['file_name']!s} — {fr['skip_reason']!s}")
            continue
        lines.append(
            f"{fr['file_name']!s}  ({fr['format']!s})  status={fr['status']!s}")
        if fr["exact_clusters"]:
            lines.append("  Known working channel-config cluster:")
            for c in fr["exact_clusters"]:
                tag = ("UNPATCHED (channel count = 12)"
                       if c["state"] == "unpatched"
                       else "ALREADY PATCHED (channel count = 8)")
                lines.append("    ["
                             f"{c['state'].upper()!s}] anchor {c['anchor_hex']!s}")
                for site in c["sites"]:
                    lines.append("      "
                                 f"{site['offset']!s} : {site['value']!s}   ("
                                 f"{site['role']!s})")
                lines.append("      -> %s" % tag)
        elif fr["status"] == "MISSING":
            lines.append("  Exact profile cluster: MISSING (different build/title, or not present)")

        # Twin-branch details (REPORT-ONLY). Shown even when the exact profile
        # is missing because this is the strongest structural lead available.
        if fr.get("twin_branches"):
            lines.append("  Twin-branch details (DBS/SM2-style channel pattern) — REPORT ONLY:")
            # cap verbose output
            for idx, tb in enumerate(fr["twin_branches"][:20], 1):
                lines.append("    [%d] Encoding:  %s" % (idx, tb["encoding"]))
                lines.append("        8-branch:  %s" % tb["eight_offset"])
                lines.append("        12-branch: %s" % tb["twelve_offset"])
                lines.append("        Gap:       %d bytes" % tb["gap"])
                inv = tb.get("investigation")
                if inv:
                    lines.append("        8-value:   "
                                 f"{inv['eight_value']!s}   12-value: {inv['twelve_value']!s}")
                    lines.append("        Region "
                                 f"{inv['region_start']!s}..{inv['region_end']!s}: "
                                 f"{inv['region_hex']!s}")
                    if inv["disassembly_available"]:
                        lines.append("        Disassembly:")
                        for d in inv["disassembly"]:
                            lines.append("          "
                                         f"{d['addr']!s}: {d['bytes']!s:14} "
                                         f"{d['mnemonic']!s} {d['op_str']!s}")
                    else:
                        lines.append("        Disassembly: (capstone not installed — showing region hex only)")
                    if inv["nearby_api_refs"]:
                        lines.append("        Nearby audio-API references (corroboration, not proof):")
                        for ref in inv["nearby_api_refs"]:
                            lines.append("          %s @ %s (%d bytes away)"
                                         % (ref["name"], ref["offset"],
                                            ref["distance"]))
                    else:
                        lines.append("        Nearby audio-API references: none within window")
                elif tb.get("region_hex"):
                    lines.append("        Region "
                                 f"{tb['region_start']!s}..{tb['region_end']!s}: "
                                 f"{tb['region_hex']!s}")
                lines.append("        Patch eligible: NO")
                lines.append("        Reason: no confirmed profile for this title/build")
            lines.append("    -> A twin-branch is the strongest lead in a scan with no "
                         "confirmed profile, but it is NOT proof and NOT "
                         "patch-eligible. Inspect the region (e.g. for "
                         "sceAudioOut2PortCreate context) before considering a "
                         "console test.")
            # Level 0 corroboration: is the audio-output API referenced at all?
            if fr.get("audio_api_strings"):
                names = ", ".join(a["name"] for a in fr["audio_api_strings"])
                lines.append("    Audio API present (corroboration, not proof): %s"
                             % names)
                lines.append("      -> The title references the audio-output API. "
                             "This raises confidence the twin-branch is "
                             "audio-related, but does NOT prove the value feeds "
                             "the API argument and does NOT authorize a patch.")
            else:
                lines.append("    Audio API string (sceAudioOut2PortCreate): not found — weaker context for this lead.")

        if fr["loose_leads"]:
            n_leads = len(fr["loose_leads"])
            has_exact = bool(fr["exact_clusters"])
            if not has_exact:
                # Collapse by default when the confirmed profile is missing:
                # these are weak leads and the list is noisy.
                lines.append("  Other possible 12-channel clusters:")
                lines.append("    %d loose lead(s) found." % n_leads)
                lines.append("    Hidden by default because the exact confirmed profile is MISSING.")
                lines.append("    These are investigation leads only, NOT patch targets.")
                lines.append("    Use Export JSON for the full list.")
            else:
                shown = min(10, n_leads)
                lines.append("  Other possible 12-channel clusters (LEADS ONLY — not patch targets):")
                for lead in fr["loose_leads"][:shown]:
                    lines.append("    %s..%s : %d× 0x0C  (investigation lead)"
                                 % (lead["start"], lead["end"],
                                    lead["count_0c"]))
                if n_leads > shown:
                    lines.append("    … %d more lead(s) not shown. Use Export JSON for the full list."
                                 % (n_leads - shown))
        lines.append("")

    lines.append("-" * 62)
    if s["savedata_module_present"]:
        lines.append("Save-data module: PRESENT in this dump.")
    else:
        lines.append("Save-data module: not present in this dump.")
    lines.append(s["savedata_note"])
    lines.append("")
    lines.append("Safe / unsafe to test patch:")
    if s["unpatched_clusters"] > 0:
        lines.append("  The EXACT cluster is present and UNPATCHED. For THIS "
                     "title/build it is a console-confirmed profile (HIGH). A 12→8 "
                     "test patch here is supported by direct working-build "
                     "evidence. For OTHER titles, verify the exact cluster is "
                     "present first — offsets are not universal.")
    elif s["patched_clusters"] > 0:
        lines.append("  The EXACT cluster is ALREADY PATCHED (8 channels). No action "
                     "needed for the channel-config fix.")
    else:
        lines.append("  Exact confirmed DBS profile: MISSING")
        lines.append("  Safe to test patch: NO")
        lines.append("  Reason: exact console-confirmed cluster not present.")
        lines.append("  Loose leads (if any) are NOT patch targets. Do not patch without ground truth for this title/build.")
    lines.append("")
    lines.append(s["disclaimer"])
    return "\n".join(lines)
