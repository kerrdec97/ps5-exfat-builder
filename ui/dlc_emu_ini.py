"""dlc_emu.ini model, parser, emitter and validators (issue: DLC-emu Phase 1).

The dlc_emu.ini format declares ONE DLC entry per [SECTION] header, and the
SAME section name (e.g. [PSAC]) may appear many times — once per entry, up to
1024. Standard configparser CANNOT represent this (it dedupes section names),
so this module models the file as an ORDERED LIST of entries and parses /
emits the text directly.

Validation rules (from the dlc_emu README):
  * file: path /app0/dlc_emu.ini; max 32 KiB; max 1024 valid entries
  * content_id: required, exactly 36 chars
  * label / entitlement_label: optional, 1-16 alphanumeric
  * service_label: optional, 1-6 alphanumeric
  * mount_point: optional, <= 15 chars (must already exist on the console;
    dlc_emu never creates it — we only WARN if it's absent locally)
  * entitlement_key: optional, 32 hex chars
  * download_status in {NO_EXTRA_DATA, NO_IN_QUEUE, DOWNLOADING,
    DOWNLOAD_SUSPENDED, INSTALLED}
  * duplicate unified or service labels -> warn (the module skips them)

This module is pure (no tkinter); it is unit-testable on its own.
"""

from __future__ import annotations

import re

# ---- limits / vocab -------------------------------------------------------
MAX_INI_BYTES = 32 * 1024
MAX_ENTRIES = 1024

SECTION_TYPES = ["NONE", "PSGD", "PSAC", "PSAL", "PSCONS", "PSVC", "PSSUBS"]
SECTION_VALUE = {
    "NONE": 0, "PSGD": 1, "PSAC": 2, "PSAL": 3, "PSCONS": 4, "PSVC": 5,
    "PSSUBS": 6,
}

DOWNLOAD_STATUSES = [
    "NO_EXTRA_DATA", "NO_IN_QUEUE", "DOWNLOADING", "DOWNLOAD_SUSPENDED",
    "INSTALLED",
]

# Known keys, in the order we emit them.
PARAM_KEYS = [
    "content_id", "label", "entitlement_label", "download_status",
    "mount_point", "entitlement_key", "service_label",
    "active_flag", "active_date", "inactive_date",
    "use_count", "use_limit", "consumable", "is_consumable",
]

_ALNUM_RE = re.compile(r"^[A-Za-z0-9]+$")
_HEX32_RE = re.compile(r"^[0-9A-Fa-f]{32}$")
_SECTION_RE = re.compile(r"^\[([A-Za-z0-9_]+)\]$")


class DlcEntry:
    """One DLC entry: a section type plus an ordered dict of params.

    Optional, NON-SERIALIZED provenance attributes (Phase 1.6) let the UI
    distinguish verified entries from best-guess/placeholder ones:
      confidence : 'high' | 'medium' | 'low' | None  (None = manual/unknown)
      source     : short human label of where the entry came from, or None
      placeholder: True if the content_id is synthesized/best-guess
    These are ignored by emit_ini / parse_ini / validate_all, so the INI
    output stays byte-identical; they exist purely for presentation.
    """

    def __init__(self, section="PSAC", params=None, confidence=None, source=None,
                 placeholder=False):
        self.section = section
        # insertion-ordered dict (py3.7+)
        self.params = dict(params or {})
        self.confidence = confidence
        self.source = source
        self.placeholder = placeholder

    def get(self, key, default=""):
        return self.params.get(key, default)

    def set(self, key, value):
        if value is None or value == "":
            self.params.pop(key, None)
            return
        self.params[key] = value

    def copy(self):
        return DlcEntry(self.section, dict(self.params),
                        confidence=self.confidence, source=self.source,
                        placeholder=self.placeholder)


def parse_ini(text):
    """Parse dlc_emu.ini text into an ordered list of DlcEntry.

    Tolerant: ignores blank lines and # / ; comments; repeated sections are
    preserved as separate entries (the whole point). Lines before the first
    section header are ignored. Unknown keys are kept verbatim so a
    round-trip never silently drops data.
    """
    entries = []
    cur = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] in "#;":
            continue
        m = _SECTION_RE.match(line)
        if m:
            cur = DlcEntry(m.group(1).upper(), {})
            entries.append(cur)
            continue
        if cur is None:
            continue
        if "=" in line:
            k, _, v = line.partition("=")
            cur.params[k.strip()] = v.strip()
    return entries


def emit_ini(entries, newline="\r\n"):
    """Render an ordered entry list back to dlc_emu.ini text.

    Emits known params in PARAM_KEYS order first, then any extra/unknown
    params in their existing order (so round-trips are lossless). Uses CRLF
    by default to match the reference example file.
    """
    blocks = []
    for e in entries:
        lines = ["[%s]" % e.section]
        seen = set()
        for k in PARAM_KEYS:
            if k in e.params and e.params[k] != "":
                lines.append(f"{k!s}={e.params[k]!s}")
                seen.add(k)
        for k, v in e.params.items():
            if k not in seen and v != "":
                lines.append(f"{k!s}={v!s}")
        blocks.append(newline.join(lines))
    text = (newline * 2).join(blocks)
    if blocks:
        text += newline
    return text


def ini_byte_size(entries, newline="\r\n"):
    return len(emit_ini(entries, newline).encode("utf-8"))


# ---- validation ------------------------------------------------------------
def validate_entry(entry, index=0, mount_exists=None):
    """Return a list of (severity, message) for one entry.

    severity is 'error' (entry would be skipped / invalid) or 'warn'
    (accepted but worth flagging). `mount_exists` may be a callable
    path->bool to check a local mount_point; None skips that check.
    """
    issues = []
    sec = (entry.section or "").upper()
    if sec not in SECTION_TYPES:
        issues.append(("error", "Unknown section type [%s]." % entry.section))

    cid = entry.get("content_id", "")
    if not cid:
        issues.append(("error", "content_id is required."))
    elif len(cid) != 36:
        issues.append(("error",
                       "content_id must be exactly 36 characters (got %d)."
                       % len(cid)))

    for lbl_key in ("label", "entitlement_label"):
        v = entry.get(lbl_key, "")
        if v and not (1 <= len(v) <= 16 and _ALNUM_RE.match(v)):
            issues.append(("error",
                           "%s must be 1-16 alphanumeric characters."
                           % lbl_key))

    sl = entry.get("service_label", "")
    if sl and not (1 <= len(sl) <= 6 and _ALNUM_RE.match(sl)):
        issues.append(("error", "service_label must be 1-6 alphanumeric characters."))

    # mount_point: <= 15 chars; must exist on console (we only warn locally)
    mp = entry.get("mount_point", "")
    if mp:
        if len(mp) > 15:
            issues.append(("error",
                           "mount_point must be at most 15 characters (got %d)."
                           % len(mp)))
        if mount_exists is not None:
            try:
                if not mount_exists(mp):
                    issues.append(("warn",
                                   'mount_point "%s" does not exist locally (it may '
                                   'exist on the console; dlc_emu will not create it).'
                                   % mp))
            except Exception:
                pass

    key = entry.get("entitlement_key", "")
    if key and not _HEX32_RE.match(key):
        issues.append(("error", "entitlement_key must be 32 hexadecimal characters."))

    # download_status
    ds = entry.get("download_status", "")
    if ds and ds not in DOWNLOAD_STATUSES:
        issues.append(("error",
                       "download_status must be one of: %s."
                       % ", ".join(DOWNLOAD_STATUSES)))

    return issues


def _unified_label(entry):
    """The unified entitlement label used for duplicate detection: explicit
    label/entitlement_label if set, else the content_id's final 16 chars."""
    for k in ("label", "entitlement_label"):
        v = entry.get(k, "")
        if v:
            return v
    cid = entry.get("content_id", "")
    return cid[-16:] if len(cid) >= 16 else cid


def validate_all(entries, mount_exists=None):
    """Validate the whole list. Returns (errors, warnings) as lists of
    'Entry N (SECTION): message' strings, including file-level limits and
    duplicate unified/service-label detection."""
    errors = []
    warnings = []

    if len(entries) > MAX_ENTRIES:
        errors.append("Too many entries: %d (max %d)."
                      % (len(entries), MAX_ENTRIES))

    size = ini_byte_size(entries)
    if size > MAX_INI_BYTES:
        errors.append("INI is %d bytes, over the 32 KiB limit (%d)."
                      % (size, MAX_INI_BYTES))

    seen_unified = {}
    seen_service = {}
    for i, e in enumerate(entries):
        tag = "Entry %d (%s)" % (i + 1, e.section)
        for sev, msg in validate_entry(e, i, mount_exists):
            (errors if sev == "error" else warnings).append(
                f"{tag!s}: {msg!s}")

        ul = _unified_label(e)
        if ul:
            if ul in seen_unified:
                warnings.append('%s: duplicate unified label "%s" (also Entry %d) '
                                '— the module skips duplicates.'
                                % (tag, ul, seen_unified[ul] + 1))
            else:
                seen_unified[ul] = i
        sl = e.get("service_label", "")
        if sl:
            if sl in seen_service:
                warnings.append('%s: duplicate service_label "%s" (also Entry %d) '
                                '— the module skips duplicates.'
                                % (tag, sl, seen_service[sl] + 1))
            else:
                seen_service[sl] = i

    return errors, warnings


# ---- eboot scan (mirrors find_codes.py; read-only) ------------------------
_CODE_RE = re.compile(rb"(?:^|\x00)([A-Z0-9]{16})(?=\x00|$)")
_SKIP_CODE = "0123456789ABCDEF"


def scan_binary_for_codes(path, content_prefix="UP9000-PPSA01234_00-"):
    """Mirror find_codes.py: scan a binary for 16-char uppercase-alnum codes
    bracketed by NUL bytes; return a list of PSAC DlcEntry stubs. READ-ONLY;
    never writes to the binary. content_prefix is editable by the caller."""
    with open(path, "rb") as f:
        data = f.read()
    seen = set()
    out = []
    for m in _CODE_RE.finditer(data):
        code = m.group(1).decode("ascii")
        if code == _SKIP_CODE or code in seen:
            continue
        seen.add(code)
        out.append(DlcEntry("PSAC", {"content_id": content_prefix + code}))
    return out


# ---- Phase 1.5: offline auto-find ----------------------------------------
import os as _os
import json as _json
import re as _re2


class Suggestion:
    """One suggested DLC entry plus provenance for the UI.

    confidence: 'high' | 'medium' | 'low'
    source:     short human label of where it came from
    entry:      a DlcEntry

    The provenance is also stamped onto the entry itself so it survives
    `entry.copy()` when the suggestion is applied to the editor.
    """

    def __init__(self, entry, confidence, source):
        self.entry = entry
        self.confidence = confidence
        self.source = source
        # Stamp provenance onto the entry (non-serialized; see DlcEntry).
        try:
            entry.confidence = confidence
            entry.source = source
        except Exception:
            pass


def _read_sfo_content_id(folder):
    """Read CONTENT_ID from a local param.sfo without importing the GUI
    module. Returns the 36-char base-game content id or ''. Pure/offline;
    minimal SFO parse (string entries only)."""
    import struct as _st
    path = ""
    for cand in (_os.path.join(folder, "sce_sys", "param.sfo"),
                 _os.path.join(folder, "param.sfo")):
        if _os.path.isfile(cand):
            path = cand
            break
    if not path:
        return ""
    try:
        with open(path, "rb") as f:
            data = f.read()
        if len(data) < 20:
            return ""
        magic, _ver, key_off, data_off, n = _st.unpack_from("<IIIII", data, 0)
        if magic != 0x46535000:
            return ""
        for i in range(n):
            eo = 20 + i * 16
            if eo + 16 > len(data):
                break
            ko, fmt, dlen, _dmax, do = _st.unpack_from("<HHIII", data, eo)
            ks = key_off + ko
            ke = data.index(b"\x00", ks)
            key = data[ks:ke].decode("utf-8", "replace")
            if key in ("CONTENT_ID", "CONTENT_ID_00"):
                vs = data_off + do
                val = data[vs:vs + dlen].rstrip(b"\x00").decode("utf-8",
                                                                 "replace")
                if len(val) == 36:
                    return val
    except Exception:
        return ""
    return ""


def looks_like_base_game_id(content_id):
    """Heuristic: does a content_id look like a base GAME id rather than DLC?

    Base-game content IDs end in a title code padded with a long run of
    trailing zeros (e.g. '...-TDF2500000000000'). Real DLC suffixes carry
    distinguishing tokens (PSAL, CONTENT, PACK, numbers) and are not mostly
    trailing zeros. Conservative: requires the 16-char suffix to end in 8+
    zeros. Used to avoid presenting the game's own id as a DLC suggestion.
    """
    if not content_id or "-" not in content_id:
        return False
    suffix = content_id.split("-")[-1]
    if len(suffix) != 16:
        return False
    trailing_zeros = len(suffix) - len(suffix.rstrip("0"))
    return trailing_zeros >= 8


def read_param_json_content_ids(folder):
    """Read DLC content IDs from a dump's param.json `disc` array, if present.

    PS5 param.json may carry a `disc` array of {contentId, contentType}
    objects (plus a shared conceptId). This is the dump's OWN declaration of
    its content IDs — no network, no guessing. Returns a list of content-id
    strings (may be empty). Read-only; tolerant of any shape / missing file.
    Separate from parse_param_json (which only reads titleId).
    """
    out = []
    try:
        path = None
        for cand in ("param.json",):
            p = _os.path.join(folder, "sce_sys", cand)
            if _os.path.isfile(p):
                path = p
                break
            p2 = _os.path.join(folder, cand)
            if _os.path.isfile(p2):
                path = p2
                break
        if not path:
            return out
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            data = _json.load(f)

        def _harvest(obj):
            if isinstance(obj, dict):
                cid = obj.get("contentId") or obj.get("content_id")
                if isinstance(cid, str) and cid:
                    out.append(cid)
                for v in obj.values():
                    _harvest(v)
            elif isinstance(obj, list):
                for v in obj:
                    _harvest(v)

        # Prefer the disc array when present; fall back to a full walk.
        if isinstance(data, dict) and isinstance(data.get("disc"), list):
            _harvest(data["disc"])
        _harvest(data)
    except Exception:
        return out
    # de-dupe, keep order
    seen = set()
    uniq = []
    for c in out:
        c = c.strip()
        if c and c not in seen:
            seen.add(c)
            uniq.append(c)
    return uniq


def detect_addcont_folders(app0):
    """Return a naturally-sorted list of existing addcont* mount paths under
    app0 (e.g. ['/app0/addcont0', '/app0/addcont1']). Detect-only; never
    creates anything."""
    found = []
    try:
        for name in _os.listdir(app0):
            full = _os.path.join(app0, name)
            if _os.path.isdir(full) and _re2.match(r"^addcont\d*$", name, _re2.I):
                found.append(name)
    except Exception:
        return []

    def _key(n):
        m = _re2.search(r"(\d+)$", n)
        return (0, int(m.group(1))) if m else (1, 0)
    found.sort(key=_key)
    return ["/app0/" + n for n in found]


def detect_content_prefix(folder, fallback_publisher="UP9000",
                          fallback_ppsa="PPSA00000"):
    """Best-effort content-id prefix like 'UP9000-PPSA01234_00-'.

    Tries param.sfo CONTENT_ID first (exact), then a content id from
    param.json's disc array, then constructs a PLACEHOLDER from PPSA. Returns
    (prefix, is_placeholder). Pure / offline; never raises.
    """
    # 1. exact prefix from the base game's CONTENT_ID
    try:
        cid = _read_sfo_content_id(folder)
        if cid and len(cid) >= 20 and "-" in cid:
            return cid[:cid.rfind("-") + 1], False
    except Exception:
        pass
    # 2. exact prefix from any param.json content id
    try:
        cids = read_param_json_content_ids(folder)
        for cid in cids:
            if len(cid) >= 20 and "-" in cid:
                return cid[:cid.rfind("-") + 1], False
    except Exception:
        pass
    # 3. placeholder built from the title id (publisher part is a guess)
    ppsa = fallback_ppsa
    try:
        from exfat_builder import get_game_info
        _t, tid, _v = get_game_info(folder)
        if tid:
            ppsa = tid
    except Exception:
        pass
    return f"{fallback_publisher!s}-{ppsa!s}_00-", True


def _content_id_suffix(cid):
    return cid[-16:] if len(cid) >= 16 else cid


# Strict PS5 title-id pattern (five digits). Case-insensitive on input.
_PPSA_RE = re.compile(r"PPSA\d{5}", re.IGNORECASE)


def detect_ppsa(app0="", entries=None, extra=None):
    """Best-effort PS5 Title ID (PPSAxxxxx) detection for the SerialStation
    lookup pre-fill.

    Priority order (first hit wins), so several disagreeing sources resolve to
    the app0 folder:
      1. app0 folder name / path      (strongest — e.g. .../PPSA02015-app)
      2. entry content_id(s)          (e.g. JP9000-PPSA02015_00-...)
      3. any other entry metadata value (title / source / label fields)
      4. extra strings                (e.g. already-mounted image metadata)

    Returns an UPPERCASE 'PPSAxxxxx' or ''. Pure and offline. Uses the strict
    pattern PPSA\\d{5} to avoid false positives; lowercase input is normalised.
    """
    def _first(s):
        if not s:
            return ""
        m = _PPSA_RE.search(str(s))
        return m.group(0).upper() if m else ""

    # 1. folder name first (leaf), then the full path
    if app0:
        s = str(app0).rstrip("/\\")
        leaf = re.split(r"[\\/]", s)[-1] if s else ""
        hit = _first(leaf) or _first(app0)
        if hit:
            return hit
    # 2. content ids
    for e in entries or []:
        cid = e.get("content_id", "") if hasattr(e, "get") else ""
        hit = _first(cid)
        if hit:
            return hit
    # 3. any other param values
    for e in entries or []:
        params = getattr(e, "params", None) or {}
        for k, val in params.items():
            if k == "content_id":
                continue
            hit = _first(val)
            if hit:
                return hit
    # 4. extra strings
    for s in extra or []:
        hit = _first(s)
        if hit:
            return hit
    return ""


def auto_find_suggestions(app0, scan_eboot=True, existing_entries=None):
    """Aggregate OFFLINE DLC suggestions for an app0 folder, in confidence
    order. Returns (suggestions, info) where info has counts and notes for the
    UI. De-dupes by content_id and against existing_entries. No network.

    Order: existing dlc_emu.ini (high) -> param.json contentIds (high) ->
    addcont folders (medium) -> eboot scan (low). param.sfo prefix feeds the
    eboot scan and any addcont-only inferences.
    """
    suggestions = []
    info = {"addcont": [], "prefix": "", "prefix_placeholder": False,
            "notes": []}
    seen_cids = set()

    def _seen(cid):
        return cid in seen_cids

    def _mark(cid):
        seen_cids.add(cid)

    # existing editor entries never get re-suggested
    if existing_entries:
        for e in existing_entries:
            c = e.get("content_id", "")
            if c:
                _mark(c)

    prefix, is_ph = detect_content_prefix(app0)
    info["prefix"] = prefix
    info["prefix_placeholder"] = is_ph

    # The base game's own CONTENT_ID is never a DLC suggestion. It is still
    # useful (prefix / notes), so remember it separately. Any id that LOOKS
    # like a base-game id (long trailing-zero suffix) is treated the same
    # way so the game's id never shows up as a bogus "DLC" row.
    base_cid = ""
    try:
        base_cid = _read_sfo_content_id(app0)
    except Exception:
        base_cid = ""
    info["base_cid"] = base_cid
    info["base_ids"] = set()
    if base_cid:
        _mark(base_cid)
        info["base_ids"].add(base_cid)

    addcont = detect_addcont_folders(app0)
    info["addcont"] = addcont

    # 1) existing dlc_emu.ini in app0 (high)
    ini_path = _os.path.join(app0, "dlc_emu.ini")
    if _os.path.isfile(ini_path):
        try:
            with open(ini_path, "r", encoding="utf-8", errors="replace") as f:
                for e in parse_ini(f.read()):
                    c = e.get("content_id", "")
                    if c and not _seen(c):
                        _mark(c)
                        suggestions.append(Suggestion(
                            e, "high", "existing dlc_emu.ini"))
        except Exception:
            pass

    # 2) param.json contentIds (high) — pair with addcont folders in order
    cids = read_param_json_content_ids(app0)
    addcont_iter = list(addcont)
    ai = 0
    for c in cids:
        if _seen(c):
            continue
        # skip the base game id (and anything shaped like one)
        if looks_like_base_game_id(c) or (base_cid and c == base_cid):
            _mark(c)
            info["base_ids"].add(c)
            continue
        _mark(c)
        e = DlcEntry("PSAC", {"content_id": c})
        if ai < len(addcont_iter):
            e.set("mount_point", addcont_iter[ai])
            e.set("download_status", "INSTALLED")
            ai += 1
        suggestions.append(Suggestion(e, "high", "param.json contentId"))

    # 3) leftover addcont folders with no declared id (medium) — synthesize a
    #    placeholder id from the prefix so the row is editable, flagged.
    for idx in range(ai, len(addcont_iter)):
        mp = addcont_iter[idx]
        # keep the 16-char suffix shape
        synth = prefix + ("ADDCONT%09d" % idx)[:16]
        if _seen(synth):
            continue
        _mark(synth)
        e = DlcEntry("PSAC", {"content_id": synth, "mount_point": mp,
                              "download_status": "INSTALLED"},
                     placeholder=True)
        suggestions.append(Suggestion(
            e, "medium", "matched to %s" % mp))

    # 4) eboot.bin scan (low) — opt-in; may be slow on big binaries
    if scan_eboot:
        try:
            root = app0
            try:
                from exfat_builder import resolve_game_root_deep
                res = resolve_game_root_deep(app0)
                if res and res.get("root"):
                    root = res["root"]
            except Exception:
                # fall back to the folder itself
                root = app0
            eboot = _os.path.join(root, "eboot.bin")
            if _os.path.isfile(eboot):
                for e in scan_binary_for_codes(eboot, content_prefix=prefix):
                    c = e.get("content_id", "")
                    if c and not _seen(c):
                        # the scan can surface the base game's own code
                        if looks_like_base_game_id(c):
                            _mark(c)
                            info["base_ids"].add(c)
                            continue
                        _mark(c)
                        # a placeholder prefix makes the whole id a guess
                        if is_ph:
                            e.placeholder = True
                        suggestions.append(Suggestion(
                            e, "low", "eboot.bin scan"))
        except Exception:
            pass

    if not suggestions:
        base_ids = info.get("base_ids") or set()
        if base_ids:
            info["notes"].append(
                "No DLC entries found locally. This dump only contains the base "
                "game content ID (%s), not add-on content. Use \"Import DLC-emu "
                "Pack\" if you have a pack, or look up the title's DLC content IDs "
                "and add them manually."
                % sorted(base_ids)[0])
        else:
            info["notes"].append("No local DLC metadata found. You can add entries manually.")

    return suggestions, info


def find_ini_in_package(pkg_path):
    """Locate a dlc_emu.ini inside a folder or .zip package. Returns the INI
    text (str) or None. Offline; never raises."""
    try:
        if _os.path.isdir(pkg_path):
            for base, _dirs, files in _os.walk(pkg_path):
                for fn in files:
                    if fn.lower() == "dlc_emu.ini":
                        with open(_os.path.join(base, fn), "r",
                                  encoding="utf-8", errors="replace") as f:
                            return f.read()
            return None
        if pkg_path.lower().endswith(".zip"):
            import zipfile as _zf
            with _zf.ZipFile(pkg_path) as z:
                for name in z.namelist():
                    if name.lower().endswith("dlc_emu.ini"):
                        return z.read(name).decode("utf-8", errors="replace")
            return None
    except Exception:
        return None
    return None


def find_modules_in_package(pkg_path):
    """List replacement-module files (.sprx/.prx) inside a package, grouped by
    variant folder when present. Returns a dict {variant: [(name, kind)]}
    where variant is 'log'/'nolog'/'' and kind is the path to copy FROM
    (folder) or the archive member name (zip). For zip, returns
    {'__zip__': zippath, variant: [member,...]}. Offline; never raises."""
    out = {}
    try:
        if _os.path.isdir(pkg_path):
            for base, _dirs, files in _os.walk(pkg_path):
                for fn in files:
                    if fn.lower().endswith((".sprx", ".prx")):
                        rel = _os.path.relpath(_os.path.join(base, fn), pkg_path)
                        parts = rel.replace("\\", "/").split("/")
                        variant = parts[0] if len(parts) > 1 and \
                            parts[0].lower() in ("log", "nolog") else ""
                        out.setdefault(variant, []).append(
                            (fn, _os.path.join(base, fn)))
            return out
        if pkg_path.lower().endswith(".zip"):
            import zipfile as _zf
            out["__zip__"] = pkg_path
            with _zf.ZipFile(pkg_path) as z:
                for name in z.namelist():
                    if name.lower().endswith((".sprx", ".prx")):
                        parts = name.replace("\\", "/").split("/")
                        variant = parts[0] if len(parts) > 1 and \
                            parts[0].lower() in ("log", "nolog") else ""
                        out.setdefault(variant, []).append((parts[-1], name))
            return out
    except Exception:
        return {}
    return out
