# Farewell Edition — development status

Branch: `farewell-edition-dev` (never `main`). Source of truth for the design:
commit `f151289` (approved redesign), preserved throughout.

This file is the resume point. When the Windows PC is reconnected, continue
from **Next steps** below rather than re-auditing.

## Evidence

| Check | Where | Result |
|---|---|---|
| Unit + GUI tests (Linux, Xvfb) | `EXFAT_GUI_SMOKE=1 python -m unittest discover -s tests` | 165 tests, OK (6 Windows-only skipped) |
| Windows build + tests | GitHub Actions `Windows build and test`, windows-2025 | `build.bat` exe build, unit tests, GUI tests, 25 pages × en/de/ja with 0 errors, exe launch (alive after 40 s); artifacts: exe + `.sha256`, screenshots |
| Real UFS2Tool on Windows | `tests/test_ufs2tool_windows.py` in CI | `.ffpkg` built into `.part`, renamed, UFS2 magic verified; ran on .NET 10.0.14 via roll-forward |
| Embedded PowerShell script | `tests/test_scripts_windows.py` in CI | parses with PowerShell's parser |
| FPKG end to end | `tests/test_fpkg.py`, GUI test | engine + add-on tests and a Build-tab job against a stand-in `prospero-pub-cmd.exe`; on Windows CI the stand-in is compiled to a real .exe (create, cancel via taskkill, update, verify/extract, non-ASCII junction build, stock-tool rejection all pass) |
| Batch file → PowerShell script | `tests/test_scripts_windows.py` in CI | extracted `make_image.bat` starts the script by full path; stops at the OSFMount check |
| Console code page | `tests/test_console_encoding_windows.py` in CI | robocopy output decodes correctly only as OEM (ANSI 1252 / OEM 437 runner) |

Not covered by any test: OSFMount image builds/mounts, robocopy on a real
game, real Sony Publishing Tools, any console behaviour.

## Technical audit (`TECHNICAL_AUDIT.md`) — status

| ID | Finding | Status |
|---|---|---|
| C1 | mkpfs rc=1 treated as success, source deleted | **Fixed** (`ui/tab_pfs.py`, test guards all PFS paths) |
| C2 | Delete-source after skipped/inconclusive verify | **Fixed** (content-verified flag; ffpkg size rule) |
| H1 | 0.5 % / 2-file verify tolerance | **Fixed** (per-file strict comparison) |
| H2 | Native mkpfs verify false failures | **Fixed** (no `--source-dir` when the build excluded files) |
| H3 | Child processes not stopped on close | **Fixed** (process-tree stop before dismount) |
| H4 | Cancel deletes a finished image | **Fixed** (finished jobs kept) |
| H5 | "N built successfully" counts queued items | **Fixed** |
| H6 | Failed ffpkg left under final name | **Fixed** (`.part` + rename) |
| M1 | "Complete!" before verification | **Fixed** |
| M2 | Rebuild deletes old image first | **Fixed** (set aside, restore on failure) |
| M3 | Widgets touched from worker threads | **Partly fixed** (thread-safe `after()` everywhere; .NET dialog moved to Tk thread); `_detect_game_info` race remains |
| M4 | Folder walks / rmtree on the UI thread | **Partly fixed** (verify size walk + delete-source rmtree moved off the Tk thread; `_run_queue` pre-walk remains) |
| M5 | Sweep dismounts the user's own mounts | **Fixed** (volumes mounted before start are left alone) |
| M6 | Updater hash optional, no timeout | **Fixed** |
| M7 | Elevated app runs code from user-writable temp | **Fixed** (private fresh folders; scripts/UFS2Tool re-checked byte-for-byte before each use; PowerShell by full path) |
| M8 | Duplicate pipelines | Open (design work) |
| M9 | Temp leftovers never recovered | **Fixed** (start-up sweep of the app's own folders older than a day) |
| M10 | Console output decoded as `mbcs` | **Fixed** (measured on Windows; OEM decoding for build/robocopy) |
| L1–L6 | Low items | L1, L2 fixed; L3 (dead retry branch) and L6 open; L4 (unused modules) needs the owner's approval to delete files |

## Issue audit (`ISSUE_AUDIT.md`) — status of defects still open there

| ID | Status |
|---|---|
| H1 finished .ffpfsc deleted when intermediate locked | **Fixed** |
| H2 unreadable image root reported as 0 files | **Fixed** |
| H3 PS1 counts skipped files | **Fixed** |
| M1 backport byte-patch fallback | **Fixed** (removed) |
| M2 stale Backport.py / sys.path | **Fixed** |
| M3 stale-mount sweep before build (#82) | Open on purpose: a sweep at build start would also unmount images the user opened in this session (File Manager / editors); needs OSFMount on Windows to judge |
| M4 right.sprx "Invalid param magic", dump fakelib | **Partly**: other tools' fakelib handled; `right.sprx` replacement needs the Backport GUI source/asset |
| M5 network-mapped letters in Python pickers | **Fixed** (`ui/shared/drives.py`, all 12 mount pickers) |
| M6 `_item_error` not logged | **Fixed** (also restores a set-aside image) |

No GitHub issue was closed or commented on. Issues that the audit found
fixed or already implemented (e.g. #60, #61, Backport GUI #28) are listed in
`ISSUE_AUDIT.md` for the owner to close.

## FPKG integration report

* **Engine:** `core/fpkg/` (original code). Modules: toolchain, param, scan,
  playgo, gp5, runner, verify, backend. Behaviour documented in
  `docs/FPKG.md`; research in `FPKG_RESEARCH.md`.
* **GUI:** Build tab → *Output format* → **FPKG**: tool path (saved as
  `fpkg_pubtools_path`), compression preset, PlayGo chunks, `img_verify`,
  keep project files. Queue job type `fpkg`.
* **Licensing:** nothing copied from the reference FPKG builder (no licence,
  ships Sony binaries); no Sony tools bundled, linked or downloaded.
* **Add-ons:** `prospero_ac` built from a content folder with its own
  `param.json` + entitlement key (dialog, `dlc_emu.ini` mapping);
  `prospero_al` refused (installer rejects it). See `docs/FPKG.md`.
* **Not done:** building from `.exfat`/`.ffpkg` directly, SELF/UCP repair, DDS→PNG. Real-tool and
  console testing. (Update builds and verify/extract of an existing `.pkg`
  are in the GUI.)

## Backport modernisation report

* **Done without the Backport GUI source:** removed the corrupting fallback;
  private, fresh engine folder; Target SDK ≤ game SDK; already-backported
  detection; other tools' fakelib backed up; free-space check; OS metadata
  files never treated as executables.
* **Done after the read-only D:\ inspection** (`core/backport_safety.py`,
  original code; tests `tests/test_backport_safety.py` and
  `BackportRunSafety` in `tests/test_backport_engine.py`):
  * originals ZIP folder required and checked **before** anything changes;
    temp originals deleted only after the ZIP was written and read back
    (Backport All used to delete them even when no originals ZIP was
    written; the single run asked for the folder only after patching, from
    the worker thread);
  * ZIPs written as `.part` + rename, never overwriting an earlier ZIP; ZIP
    errors and decrypt/sign failures reported as failures;
  * replaced fakelib files of another tool now really in the originals ZIP;
  * ZIP folder inside the game, ZIP/temp paths over 259 characters, and
    unusable fakelib sets (sub-folders, < 1 KiB, non-ELF/SELF, inside the
    game) refused;
  * already-backported detection from patched-file hashes recorded in the
    profile manifest (older manifests: "differs from recorded originals"
    asks, Backport All skips);
  * Backport All runs the same per-game checks; restore refuses zip-slip
    entries and reports partial restores.

### Backport GUI audit findings compared with this repo

The Backport GUI checkout (`D:\fixing input\unity_input_gui`, branch
`snapshot/20261021-f1-pro-v45`, HEAD `3cf4ffac`, seven unstaged changes) was
inspected read-only on the owner's PC and left unchanged. Nothing from it was
copied. This repo's Backport engine is the embedded `Backport.py` pipeline
(decrypt → SDK pair 1–10 → fake-sign), not the Backport GUI engine, so most
findings concern code that does not exist here.

| Audit finding (Backport GUI) | In this repo? | Result |
|---|---|---|
| Import-table checks FW 4.03–13.60 skip decode errors, merge gaps, trust seeded slots | No import-table audit here | Not ported: the firmware import tables have undocumented provenance. A clean-room version needs tables built from a documented source |
| V2 registry / report-only patterns / firmware boundaries / planner families disagree; old AGENTS.md count | None of these exist here (no `AGENTS.md`, no planner) | Nothing consumed here; nothing to reconcile |
| Audio gate checks only `sceAudioOut2Set3DLatency`; live parser keeps last 600 chars | No such gate or truncation (`ui/audio_fix_scan.py` checked) | Not applicable |
| AMPR `prtm` (JWE3 V32 kernel panic after ~1 min) / `prtp` (fails on Planet Coaster 2 V3) | Not present; `ui/build_ampr_index.py` only builds the path index | Not added. If ever added, experimental and off by default |
| `packaged_ampr_sha` / `ampr_unmap_prt_fatal` leave ZIPs open | Not present; every `ZipFile` here is a `with` block (incl. embedded `Archive.py`) | New ZIP helpers tested for closed handles (rename + delete after use) |
| Already-backported detection filename/coarse based, no tests | Fakelib-content check existed with tests | **Fixed** further: manifest hash detection, Backport All coverage |
| Destination: nested output, Windows long paths | Missing | **Fixed** (see above) |
| Release-only 4e1d7250 rejects nested/undersized fake libraries | Reproduced: a nested or empty set was copied as-is | **Fixed** independently (sub-folder, < 1 KiB, non-ELF/SELF) |
| Boundary UNKNOWN → HARDWARE_MEASURED_FW13_20_PRO | No firmware boundary model here | Record only: the evidence is FW 4.50 Base rejection and Pro 13.20. Other models/firmwares are untested |
| SDK5–13 target pairs | This engine supports pairs 1–10 | Hardware proof reported for SDK 5 only; no higher pairs added |

### Blocked on licence / provenance (kept out of this repo)

* The Backport GUI checkout has no tracked project licence, so none of its
  code is reused.
* A GPL-3.0 AMPR/PlayGo source ships there without the licence text or a
  source offer; not copied or bundled.
* `right.sprx` (Sony-derived), PSSR and kernel files (game-derived) and the
  firmware import tables (undocumented origin) are not copied, bundled or
  downloaded. The `right.sprx` "Invalid param magic" fix (issue audit M4)
  stays open for that reason.

## Remaining backport gaps

* No test runs the real embedded pipeline on real PS5 executables (no
  license-clear sample SELFs); the run-safety tests use a stand-in temp tree.
* The embedded pipeline's SDK patcher can make `.bak` files beside the
  files it patches; whether any reach the game folder in Auto Backport is
  unverified without real executables.
* SDK Fixer / Batch SDK (separate tools) were not part of this pass.
* No PS5 hardware or real Publishing Tools available to this session.

## Next steps (resume here)

1. **Backport GUI:** inventory done (read-only). Only clean-room,
   license-clear equivalents are allowed; the import-table audit needs
   firmware tables from a documented source first. `right.sprx`, PSSR,
   kernel files and the GPL AMPR/PlayGo source stay out.
2. On a real Windows PC with OSFMount: build one small game end to end
   (exFAT, FFPKG, PFS), verify, extract; confirm the rebuild/restore path
   and the close-during-build behaviour.
3. FPKG with the owner's Publishing Tools: one small game, then a
   multi-language game; record results in `docs/FPKG.md`.
4. Remaining audit items: M4 (queue pre-walk), M8 (duplicate pipelines), L3/L6, issue-audit M3; L4 deletion needs approval.
5. Release: only with the owner's approval (no public release or push to
   `main` without it). The CI exe artifact is a test build, not a release.
