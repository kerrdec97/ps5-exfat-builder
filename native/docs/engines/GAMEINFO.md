# gameinfo — dump detection, title metadata, covers, Library, Dump Rename

Target `fw_engine_gameinfo` (`native/src/engines/gameinfo/`). Other modules
(the image engines `exfat`, `ufs2` and `pfs`, `tools`, and through them the
Library and Build pages) depend on it, so the API is kept small. The backport
helpers deliberately do not (`src/engines/backport/CMakeLists.txt`). Links Qt
Core and **Qt Gui** (only for `QImage` in cover normalisation; no widgets, no
`QGuiApplication` needed).

Legacy references: `E:` = `exfat_builder.py` of the Python app (versions 1 to
4, no longer in this repository). Everything below was checked
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

The four files are located first, by the `findMetaFile` rule for each: root, `sce_sys`, then a depth-first walk ≤ 5 levels and at most 20,000 folders, case-insensitive, links not followed. `readGameMetadata` does **one** walk for all files that are not at the root or in `sce_sys` (each file is the one a walk for it alone would find); the walk stops once `param.sfo`, `pfs-version.dat` and `param.json` are found, and `nptitle.dat` gets a walk of its own only when it is needed and was not seen by then. The optional `CancellationToken` is checked per folder; a cancelled read returns `Cancelled`.

1. `param.sfo`: title = `TITLE` or `TITLE_00`; ID = `TITLE_ID` (ignored when stored as an integer); version = `VERSION` or `APP_VER`. At most 4,096 entries (more: the file is refused); a value over 64 KiB, or beyond a total of the file's own size, is skipped like any other damaged entry.
2. `pfs-version.dat`: first line of the file; if it starts with a digit or `.` it **overrides** the version (legacy `re.match(r'[\d.]+')`).
3. `param.json` is used for title/ID only when the SFO gave **neither** title nor ID; its version only when neither the SFO nor pfs-version gave one. Title order: `localizedParameters[defaultLanguage]` → `en-US` → `en-GB` → first language **in file order** with a `titleName` → top-level `titleName` → `name`. Version = `version` or `masterVersion`.
4. `nptitle.dat`: first line, title only, when there is still no title.
5. Version normalised (`"01.02"` → `"01.002.000"`, `"1.007.000"` → `"01.007.000"`; legacy `zfill` 2/3/3), title sanitised.

Extra facts (not in the legacy chain): `contentId` (param.json, else SFO `CONTENT_ID`), `contentVersion`, `sdkVersion` (param.json keys `sdkVersion, sdk_version, SDK_VER, requiredSystemSoftwareVersion, systemVersion, SYSTEM_VER` in that order, legacy `_read_sfo_info`; else SFO `SYSTEM_VER/SDK_VER/SYSTEM_VERSION`), `requiredSystemVersion` (param.json `requiredSystemSoftwareVersion`, else SFO `SYSTEM_VER`), the parsed files and `notes` for damaged/repaired files. It never fails for an existing folder (other than `Cancelled`, when a token is passed). JSON numbers outside the integer range are ignored.

### Platform and kind (only where determinable)

| Result | Rule |
|---|---|
| PS5 | the dump's own `param.json` (at the root or in `sce_sys`, even a damaged one); else title ID `PPSA…`; else a `param.json` found only by the deep walk, unless a PS4 rule below applies first; else `eboot.bin` starting with the retail PS5 SELF magic `54 14 F5 EE` |
| PS4 | title ID `CUSA…`, or the dump's own `param.sfo` (root or `sce_sys`), both of which outweigh a `param.json` found only deeper down (a common name for game configuration files); else a `param.sfo` without any `param.json` |
| Game / Patch / Add-on (PS4) | SFO `CATEGORY` `gd`/`gde` / `gp` / `ac` |
| Add-on (PS5) | `param.json` with no `eboot.bin` (an add-on content folder carries its own `sce_sys/param.json`, `core/fpkg/dlc.py`) |
| Game (PS5) | `eboot.bin` + `param.json` with `applicationCategoryType` 0 or absent |
| Game (platform as decided above) | `eboot.bin` with neither a parsed `param.json` nor PS4 SFO rules (reason `eboot.bin`) |
| Unknown | anything else (e.g. other `applicationCategoryType` values, or no `eboot.bin` and no metadata). A PS5 **patch** is not detectable from a dump folder; it reads as a game. |

`kindReason` says which rule decided.

### Versions

`decodeSystemVersion(u32)`: top byte = major, next byte = minor, both BCD (`0x04508001` → 4.50, `0x10000000` → 10.00). A zero top byte shifts one byte (legacy "shift 24, then 16"). Non-BCD bytes fall back to the legacy numeric reading with the legacy plausibility range 1..30. `parseVersionField()` is the legacy `_parse_ps5_version_field` (hex characters read as digits; non-BCD hex → numeric top byte; decimal 1..99), plus JSON numbers > 99 read as the packed quad word.

## Detection helpers

* `findGameRoots()` = legacy `resolve_game_root_deep` (BFS, depth 5, 4000 folders, skips dot folders and `sce_sys sce_module fakelib image $recycle.bin system volume information __macosx`; a folder holding `eboot.bin` is a candidate and is not descended into; exactly one candidate = root).
* `findLibraryGameRoot()` = Library rule: `eboot.bin` in the wrapper, a child, or a grandchild.
* `findRenameGameRoot()` = `_dumps_find_eboot_root`: itself, a child with `eboot.bin`, or the folder when its name contains a title ID.
* `dumpSize()` = `_get_folder_size`: sum of regular files, symlinks and junctions not followed.
* `findCoverSource()` = `_load_cover_art`: `sce_sys/icon0.png`, `icon0.png`, then ≤ 4 levels (at most 20,000 folders) skipping `decrypted` and `fakelib`.

Children are visited in name order (legacy used unsorted `os.scandir` order).

## Naming

* `sanitizeTitle()` = legacy `sanitize_filename` (E:275): curly quotes/dashes/ellipsis normalised, ®©™℠ dropped, bullets to spaces, C0/C1 controls dropped, `\ / : * ? " < > |` and `& ^ % ! \` $` removed, whitespace collapsed, leading/trailing spaces **and dots** trimmed (so a trailing `…` disappears, as in the legacy), 80 code points (never splitting a surrogate pair), `game` when nothing is left.
* Dump Rename presets (legacy keys): `ppsa` → `PPSA01234`; `ppsa_title` → `PPSA01234 Spider-Man`; `ppsa_title_v` → `PPSA01234 Spider-Man (01.050.000)`. ID upper-cased and sanitising on (the legacy defaults; neither had a UI). The whole name is capped at 80 code points; when it would be longer, the **title** is shortened so the version at the end stays whole (the legacy cut the end of the joined name, version included).
* `buildImageBaseName()` = `build_exfat_name` without `.exfat`.
* `renderImageName()` = the legacy Build tab `_gen_name` (`ui/tab_unibuild.py:1318`) used by the Build page: presets `ppsa`, `ppsa_title`, `ppsa_title_ver` (default), `ppsa_title_ver_fw` (`… (FW<major>)`, firmware = the dump's `requiredSystemSoftwareVersion` or `build.firmwareOverride`), `folder` (the source folder's name, #43) and `custom` (template tokens `%TITLE% %PPSA% %VERSION% %FIRMWARE% %TYPE% %DATE%` plus `%FOLDER% %SDK%`, case-insensitive, replaced in one pass, an unknown `%WORD%` left as typed with its closing `%` free to open the next token; then empty `()`/`[]`/`{}` removed in up to 4 passes, whitespace collapsed, spaces inside brackets trimmed, ` -_–—` stripped at both ends). Empty result → the folder name → `game`. Cleaned with `sanitizeImageName()`: unlike `sanitizeTitle()` it keeps Unicode letters, apostrophes and `& ^ % ! $` and removes only what Windows forbids (`< > : " / \ | ? *`, control characters), trims spaces/dots, caps at 150 code points and puts `_` after a reserved device name (`NUL` → `NUL_`, `NUL.backup` → `NUL_.backup`).
* `isValidContentId()` = `[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}`, the whole string (36 characters, `core/fpkg/param.py`; a trailing line feed is not accepted); `titleIdFromContentId()` = characters 8–16. These two and `isValidTitleId()` / `buildImageBaseName()` are used by the tests only; the app validates content IDs with `fpkg::isValidContentId` and the DLC module's own, wider patterns.

## Covers (legacy `_normalize_cover` E:721)

source → transparent-border trim → bounded solid-border trim (tolerance 12, corners must agree, the legacy bounds including its 75 % right-side bound) → centre crop to the cover's 2:3 shape → scale to 260×390 → PNG (cropping first gives the same picture as the legacy scale-then-crop and keeps the work bounded for a very long or very tall image). Only PNG/JPEG/WEBP/BMP/DDS (whatever of those the installed Qt image plugins decode — PNG/BMP built in, JPEG plugin; WEBP/DDS need qtimageformats) are accepted, detected from the content; images of more than 8192×8192 pixels in total (67 megapixels, whatever the shape) are refused from the header before decoding. Cache file: `<cache>/<sha1("<abs native path>|<mtime s>|<size>|260x390")>.png`, written atomically. Scaling uses Qt's smooth (bilinear/area) filter, not Lanczos, so pixels differ slightly from the legacy cache.

## Library scan (`library.scan`, legacy `_lib_scan` E:22015)

Library folders → every immediate sub-folder ("wrapper"), skipping `$sysreset found.000 lost+found $windows.~bt $windows.~ws recovery recycler $recycle.bin system volume information` and dot folders → game root by the 3-level rule (no `eboot.bin` = not a game) → metadata → title ID fallback from the root's then the wrapper's folder name → size, cover, SDK fallback from the wrapper → dedupe by normalised title ID (bigger dump, then shallower wrapper, then shallower root; games without an ID all kept) → sorted by title, case-insensitively. Missing library folders are warnings.

Job parameters: `folders` (array, default the `library.folders` setting), `measureSizes` (default true), `covers` (default true). Outcome: `details.games` (array of `LibraryGame::toJson()`), `outputs` = cover PNGs, summary "N game(s) found across M folder(s)" (legacy wording).

## Dump Rename (`library.rename`, legacy `ui/dump_rename.py`, E:22993)

| Mode | Behaviour |
|---|---|
| `preview` (default) | dry run: the plan (`details.items`: current and proposed name, target, title/ID/version, legacy confidence green/yellow/red, status). Nothing on disk changes. |
| `apply` | re-plans from disk, applies `items` edits (`{source, name, selected}`; paths compared without letter case on Windows), re-checks, renames. When `items` is given, a dump that no edit names is left alone (it was not in the preview). Without `items`, every dump is renamed with its automatic name. |
| `undo` | reverses an undo log, newest first. |

Outcomes: `details.items`, `renamed`, `skipped`, `failed` and `undoLog` (only when the log was written). Verification is **Passed** only when everything asked for was done. A cancel after at least one rename (or restored name) returns the outcome with verification **Inconclusive**, a summary starting "Cancelled after…" and a warning for each dump that was not reached; a cancel before the first one is a plain `Cancelled`. An undo entry whose original name is taken is not an error but is counted ("N not restored", Inconclusive). A rename that failed (folder in use) makes verification **Failed**, with the outcome still attached to the job.

Safety rules: never overwrite (an existing target — file or folder — is "Skipped — target exists", checked again right before each rename); two dumps proposing the same name: the second is skipped; edited names Windows would reject (forbidden characters, reserved device names, trailing dot/space) are refused; a case-only change on Windows counts as "Already named" (legacy `normcase`); a destination folder (`destination` parameter) must be on the same drive (rename, never copy+delete). Every rename is written to the undo log before it happens, as a `"pending": true` entry that loses the flag once the folder has its new name (`<data>/rename-undo/rename-<time>.json`, or `undoLog`), so a crash between the two still leaves the old name on record; undo reverses a pending entry whose new name exists and passes over one whose rename never happened, and a rename that fails is taken out of the log again; undo never overwrites either and marks entries as undone so a second undo is a no-op. Verification after apply: each renamed dump exists at its new path and no longer at the old one.

Parameters: `mode`, `source`, `preset` (default the `library.renamePreset` setting), `destination`, `items`, `undoLog`.

## Settings

| Key | Section | Type / default | Read by | Legacy key |
|---|---|---|---|---|
| `library.folders` | General › Library | string list, empty | The Library page's folders (its Add folder… and Remove change this setting; `LibraryStore` keeps a copy in `library.json`; Reset section and Reset all leave it, `keptOnReset`) and `library.scan` when `folders` is not given | `lib_folders` (added to the folders already there) |
| `library.renamePreset` | General › Library | enum `ppsa` / `ppsa_title` / `ppsa_title_v`, `ppsa_title_v` | `library.rename` when `preset` is not given | — (the legacy radio was not persisted; code default `ppsa_title_v`) |
| `library.coverCacheFolder` | Storage › Folders (advanced) | folder, empty = `<local app data>/cache/covers` | `library.scan` | — (legacy fixed `~/exfat_builder_logs/covers_norm`) |
| `build.namePreset` | Build › Output name | enum `ppsa_title_ver` / `ppsa_title_ver_fw` / `ppsa_title` / `ppsa` / `folder` / `custom`, `ppsa_title_ver` | Build page file name (`renderImageName`) | `name_preset` |
| `build.nameTemplate` | Build › Output name (enabled for `custom`) | string, `%TITLE% (%VERSION%) (%FIRMWARE%) (%PPSA%)` | Build page file name | `name_custom_template` |
| `build.firmwareOverride` | Build › Output name (advanced) | enum `auto` / `1`…`15`, `auto` | `%FIRMWARE%` and the firmware preset | — (the legacy Build tab override was not persisted) |

`lib_auto_scan` and `dumps_last_src` are UI-side and not declared here (no consumer in this module).

## Deliberate differences from the legacy app

* A dump with no title ID gets **no** proposed name (the legacy showed "Cannot rename" but its worker would still rename it to the bare title).
* `nptitle.dat` whose first line contains a NUL byte (binary) is not used as a title (the legacy would have used the garbage).
* SFO `SYSTEM_VER` is decoded as BCD (the legacy read the top byte as a plain number, so SDK 10 showed as 16). The format `0x0004` (UTF-8 special) counts as a string.
* `param.json` with a BOM, comments, trailing commas or NUL padding is read (the legacy `json.load` failed on those); damaged SFO entries are skipped instead of discarding the whole file.
* Library dedupe/sort, cover search and root search visit folders in name order.
* Rename: undo log, duplicate detection, invalid-name check and same-drive destination are new.

## Tests (`native/tests/engines/gameinfo/tst_gameinfo.cpp`, links only `fw_engine_gameinfo`)

Synthetic dumps in a temporary folder; `param.sfo` fixtures are written by an independent writer in the test. Covered: BCD decoding and version fields; content-version padding; content/title ID rules; the sanitiser (typographic characters, cmd metacharacters, Japanese and emoji titles, 80-code-point cap); the three presets; SFO strings/integers, bad magic, out-of-range entries, truncated tables; param.json fields, title fallback order including file order, BOM/comments/trailing commas/NUL padding/invalid UTF-8/UTF-16, garbage; the JSON scanner's order and byte-exact edits; the full chain priorities (SFO vs param.json vs pfs-version vs nptitle, binary nptitle, integer TITLE_ID); deep and case-insensitive meta files; damaged files as notes; platform/kind rules; root searches incl. depth and folder limits; sizes without following symlinks and cancellation; cover trim/fill/cache/refusals (non-image, 9000×9000 header); a two-folder library scan with dedupe, PS4 + PS5 dumps, folder-name ID fallback and skipped OS folders; the scan job reading its settings; rename plan/apply/undo including never-overwrite after planning, duplicates, invalid edits, destination folder and the job's preview/apply/undo modes; undo with a taken original name and a cancel part-way; a cancelled apply keeping its outcome and undo log; apply touching only the dumps its edits name; the single cancellable metadata walk; a stray `param.json` in a PS4 dump; long names keeping their version, odd templates and reserved names; hostile input (a `param.sfo` with a million entries, oversized and overlapping values; 1×8192 and 8192×1 covers; JSON numbers out of range); settings well-formedness and the `lib_folders` import.

Not tested: WEBP/DDS covers (no plugin here), real dumps.
