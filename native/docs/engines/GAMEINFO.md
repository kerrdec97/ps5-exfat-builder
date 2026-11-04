# gameinfo — dump detection, title metadata, covers, Library, Dump Rename

Target `fw_engine_gameinfo` (`native/src/engines/gameinfo/`). Other modules
(Library, Backport, Build, `fw_connect_ps5`) depend on it, so the API is kept
small. Links Qt Core and **Qt Gui** (only for `QImage` in cover normalisation;
no widgets, no `QGuiApplication` needed).

Legacy references: `E:` = `exfat_builder.py`. Everything below was checked
against the legacy code, not only against `LEGACY_UI_INVENTORY.md`.

## Headers

| Header | What it gives |
|---|---|
| `GameInfo.h` | `readGameMetadata()` (the legacy chain + platform, kind, SDK, firmware, content ID), `findMetaFile()`, `findGameRoots()`, `findLibraryGameRoot()`, `findRenameGameRoot()`, `hasEbootBin()`, `dumpSize()`, `findCoverSource()` |
| `Sfo.h` | `parseSfo()` / `readSfo()` — PS4/PS5 `param.sfo` |
| `ParamJson.h` | `parseParamJson()` / `readParamJson()` — lenient `param.json`, raw object kept |
| `Version.h` | `SystemVersion`, `decodeSystemVersion()` (0x04508001 → "4.50"), `parseVersionField()`, `normalizeContentVersion()` |
| `Naming.h` | content/title ID checks, `sanitizeTitle()`, the three Dump Rename presets, `buildDumpName()`, `buildImageBaseName()`, `folderNameProblem()` |
| `Cover.h` | `normalizedCover()` (260×390 PNG in the cache), `normalizeCoverImage()`, `trimCoverBorders()` |
| `Library.h` | `scanLibrary()`, `dedupeLibrary()`, `LibraryGame` (+ JSON round trip) |
| `DumpRename.h` | `planDumpRenames()`, `refreshRenamePlan()`, `applyDumpRenames()`, `undoDumpRenames()` |
| `JsonText.h` | order-preserving JSON member scanner, byte-exact value replacement, lenient repair (also used by Y2JB edits) |
| `Module.h` | `registerModule()`, setting keys, job kinds |

## Metadata chain (`readGameMetadata`, legacy `get_game_info` E:442)

1. `param.sfo` found by `findMetaFile` (root, `sce_sys`, then a depth-first walk ≤ 5 levels, case-insensitive): title = `TITLE` or `TITLE_00`; ID = `TITLE_ID` (ignored when stored as an integer); version = `VERSION` or `APP_VER`.
2. `pfs-version.dat`: first line of the file; if it starts with a digit or `.` it **overrides** the version (legacy `re.match(r'[\d.]+')`).
3. `param.json` is used for title/ID only when the SFO gave **neither** title nor ID; its version only when neither the SFO nor pfs-version gave one. Title order: `localizedParameters[defaultLanguage]` → `en-US` → `en-GB` → first language **in file order** with a `titleName` → top-level `titleName` → `name`. Version = `version` or `masterVersion`.
4. `nptitle.dat`: first line, title only, when there is still no title.
5. Version normalised (`"01.02"` → `"01.002.000"`, `"1.007.000"` → `"01.007.000"`; legacy `zfill` 2/3/3), title sanitised.

Extra facts (not in the legacy chain): `contentId` (param.json, else SFO `CONTENT_ID`), `contentVersion`, `sdkVersion` (param.json keys `sdkVersion, sdk_version, SDK_VER, requiredSystemSoftwareVersion, systemVersion, SYSTEM_VER` in that order, legacy `_read_sfo_info`; else SFO `SYSTEM_VER/SDK_VER/SYSTEM_VERSION`), `requiredSystemVersion` (param.json `requiredSystemSoftwareVersion`, else SFO `SYSTEM_VER`), the parsed files and `notes` for damaged/repaired files. It never fails for an existing folder.

### Platform and kind (only where determinable)

| Result | Rule |
|---|---|
| PS5 | a readable `param.json`; else title ID `PPSA…`; else `eboot.bin` starting with the retail PS5 SELF magic `54 14 F5 EE` |
| PS4 | title ID `CUSA…`, or a `param.sfo` without any `param.json` |
| Game / Patch / Add-on (PS4) | SFO `CATEGORY` `gd`/`gde` / `gp` / `ac` |
| Add-on (PS5) | `param.json` with no `eboot.bin` (an add-on content folder carries its own `sce_sys/param.json`, `core/fpkg/dlc.py`) |
| Game (PS5) | `eboot.bin` + `param.json` with `applicationCategoryType` 0 or absent |
| Unknown | anything else (e.g. other `applicationCategoryType` values). A PS5 **patch** is not detectable from a dump folder; it reads as a game. |

`kindReason` says which rule decided.

### Versions

`decodeSystemVersion(u32)`: top byte = major, next byte = minor, both BCD (`0x04508001` → 4.50, `0x10000000` → 10.00). A zero top byte shifts one byte (legacy "shift 24, then 16"). Non-BCD bytes fall back to the legacy numeric reading with the legacy plausibility range 1..30. `parseVersionField()` is the legacy `_parse_ps5_version_field` (hex characters read as digits; non-BCD hex → numeric top byte; decimal 1..99), plus JSON numbers > 99 read as the packed quad word.

## Detection helpers

* `findGameRoots()` = legacy `resolve_game_root_deep` (BFS, depth 5, 4000 folders, skips dot folders and `sce_sys sce_module fakelib image $recycle.bin system volume information __macosx`; a folder holding `eboot.bin` is a candidate and is not descended into; exactly one candidate = root).
* `findLibraryGameRoot()` = Library rule: `eboot.bin` in the wrapper, a child, or a grandchild.
* `findRenameGameRoot()` = `_dumps_find_eboot_root`: itself, a child with `eboot.bin`, or the folder when its name contains a title ID.
* `dumpSize()` = `_get_folder_size`: sum of regular files, symlinks and junctions not followed.
* `findCoverSource()` = `_load_cover_art`: `sce_sys/icon0.png`, `icon0.png`, then ≤ 4 levels skipping `decrypted` and `fakelib`.

Children are visited in name order (legacy used unsorted `os.scandir` order).

## Naming

* `sanitizeTitle()` = legacy `sanitize_filename` (E:275): curly quotes/dashes/ellipsis normalised, ®©™℠ dropped, bullets to spaces, C0/C1 controls dropped, `\ / : * ? " < > |` and `& ^ % ! \` $` removed, whitespace collapsed, leading/trailing spaces **and dots** trimmed (so a trailing `…` disappears, as in the legacy), 80 code points (never splitting a surrogate pair), `game` when nothing is left.
* Dump Rename presets (legacy keys): `ppsa` → `PPSA01234`; `ppsa_title` → `PPSA01234 Spider-Man`; `ppsa_title_v` → `PPSA01234 Spider-Man (01.050.000)`. ID upper-cased and sanitising on (the legacy defaults; neither had a UI).
* `buildImageBaseName()` = `build_exfat_name` without `.exfat`.
* `isValidContentId()` = `^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}$` (36 characters, `core/fpkg/param.py`); `titleIdFromContentId()` = characters 8–16.

## Covers (legacy `_normalize_cover` E:721)

source → transparent-border trim → bounded solid-border trim (tolerance 12, corners must agree, the legacy bounds including its 75 % right-side bound) → scale to fill 260×390 → centre crop → PNG. Only PNG/JPEG/WEBP/BMP/DDS (whatever of those the installed Qt image plugins decode — PNG/BMP built in, JPEG plugin; WEBP/DDS need qtimageformats) are accepted, detected from the content; images over 8192×8192 are refused from the header before decoding. Cache file: `<cache>/<sha1("<abs native path>|<mtime s>|<size>|260x390")>.png`, written atomically. Scaling uses Qt's smooth (bilinear/area) filter, not Lanczos, so pixels differ slightly from the legacy cache.

## Library scan (`library.scan`, legacy `_lib_scan` E:22015)

Library folders → every immediate sub-folder ("wrapper"), skipping `$sysreset found.000 lost+found $windows.~bt $windows.~ws recovery recycler $recycle.bin system volume information` and dot folders → game root by the 3-level rule (no `eboot.bin` = not a game) → metadata → title ID fallback from the root's then the wrapper's folder name → size, cover, SDK fallback from the wrapper → dedupe by normalised title ID (bigger dump, then shallower wrapper, then shallower root; games without an ID all kept) → sorted by title, case-insensitively. Missing library folders are warnings.

Job parameters: `folders` (array, default the `library.folders` setting), `measureSizes` (default true), `covers` (default true). Outcome: `details.games` (array of `LibraryGame::toJson()`), `outputs` = cover PNGs, summary "N game(s) found across M folder(s)" (legacy wording).

## Dump Rename (`library.rename`, legacy `ui/dump_rename.py`, E:22993)

| Mode | Behaviour |
|---|---|
| `preview` (default) | dry run: the plan (`details.items`: current and proposed name, target, title/ID/version, legacy confidence green/yellow/red, status). Nothing on disk changes. |
| `apply` | re-plans from disk, applies `items` edits (`{source, name, selected}`), re-checks, renames. |
| `undo` | reverses an undo log, newest first. |

Safety rules: never overwrite (an existing target — file or folder — is "Skipped — target exists", checked again right before each rename); two dumps proposing the same name: the second is skipped; edited names Windows would reject (forbidden characters, reserved device names, trailing dot/space) are refused; a case-only change on Windows counts as "Already named" (legacy `normcase`); a destination folder (`destination` parameter) must be on the same drive (rename, never copy+delete). Every completed rename is appended to the undo log immediately (`<data>/rename-undo/rename-<time>.json`, or `undoLog`); undo never overwrites either and marks entries as undone so a second undo is a no-op. Verification after apply: each renamed dump exists at its new path and no longer at the old one.

Parameters: `mode`, `source`, `preset` (default the `library.renamePreset` setting), `destination`, `items`, `undoLog`.

## Settings

| Key | Section | Type / default | Read by | Legacy key |
|---|---|---|---|---|
| `library.folders` | General › Library | string list, empty | `library.scan` when `folders` is not given | `lib_folders` |
| `library.renamePreset` | General › Library | enum `ppsa` / `ppsa_title` / `ppsa_title_v`, `ppsa_title_v` | `library.rename` when `preset` is not given | — (the legacy radio was not persisted; code default `ppsa_title_v`) |
| `library.coverCacheFolder` | Storage › Folders (advanced) | folder, empty = `<local app data>/cache/covers` | `library.scan` | — (legacy fixed `~/exfat_builder_logs/covers_norm`) |

`lib_auto_scan` and `dumps_last_src` are UI-side and not declared here (no consumer in this module).

## Deliberate differences from the legacy app

* A dump with no title ID gets **no** proposed name (the legacy showed "Cannot rename" but its worker would still rename it to the bare title).
* `nptitle.dat` whose first line contains a NUL byte (binary) is not used as a title (the legacy would have used the garbage).
* SFO `SYSTEM_VER` is decoded as BCD (the legacy read the top byte as a plain number, so SDK 10 showed as 16). The format `0x0004` (UTF-8 special) counts as a string.
* `param.json` with a BOM, comments, trailing commas or NUL padding is read (the legacy `json.load` failed on those); damaged SFO entries are skipped instead of discarding the whole file.
* Library dedupe/sort, cover search and root search visit folders in name order.
* Rename: undo log, duplicate detection, invalid-name check and same-drive destination are new.

## Tests (`native/tests/engines/gameinfo/tst_gameinfo.cpp`, links only `fw_engine_gameinfo`)

Synthetic dumps in a temporary folder; `param.sfo` fixtures are written by an independent writer in the test. Covered: BCD decoding and version fields; content-version padding; content/title ID rules; the sanitiser (typographic characters, cmd metacharacters, Japanese and emoji titles, 80-code-point cap); the three presets; SFO strings/integers, bad magic, out-of-range entries, truncated tables; param.json fields, title fallback order including file order, BOM/comments/trailing commas/NUL padding/invalid UTF-8/UTF-16, garbage; the JSON scanner's order and byte-exact edits; the full chain priorities (SFO vs param.json vs pfs-version vs nptitle, binary nptitle, integer TITLE_ID); deep and case-insensitive meta files; damaged files as notes; platform/kind rules; root searches incl. depth and folder limits; sizes without following symlinks and cancellation; cover trim/fill/cache/refusals (non-image, 9000×9000 header); a two-folder library scan with dedupe, PS4 + PS5 dumps, folder-name ID fallback and skipped OS folders; the scan job reading its settings; rename plan/apply/undo including never-overwrite after planning, duplicates, invalid edits, destination folder and the job's preview/apply/undo modes; settings well-formedness and the `lib_folders` import.

Not tested: WEBP/DDS covers (no plugin here), Windows paths and case-insensitive renames (Linux only), real dumps.
