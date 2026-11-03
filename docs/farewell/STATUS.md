# Farewell Edition — development status

Branch: `farewell-edition-dev` (never `main`). Source of truth for the design:
commit `f151289` (approved redesign), preserved throughout.

This file is the resume point. When the Windows PC is reconnected, continue
from **Next steps** below rather than re-auditing.

## Evidence

| Check | Where | Result |
|---|---|---|
| Unit + GUI tests (Linux, Xvfb) | `EXFAT_GUI_SMOKE=1 python -m unittest discover -s tests` | 118 tests, OK (3 Windows-only skipped) |
| Windows build + tests | GitHub Actions `Windows build and test`, windows-2025 | `build.bat` exe build, unit tests, GUI tests, 25 pages × en/de/ja with 0 errors, exe launch (alive after 40 s); artifacts: exe + `.sha256`, screenshots |
| Real UFS2Tool on Windows | `tests/test_ufs2tool_windows.py` in CI | `.ffpkg` built into `.part`, renamed, UFS2 magic verified; ran on .NET 10.0.14 via roll-forward |
| Embedded PowerShell script | `tests/test_scripts_windows.py` in CI | parses with PowerShell's parser |
| FPKG end to end | `tests/test_fpkg.py`, GUI test | 31 engine tests + one Build-tab job, against a stand-in `prospero-pub-cmd.exe` |

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
| M1 | "Complete!" before verification | Open |
| M2 | Rebuild deletes old image first | **Fixed** (set aside, restore on failure) |
| M3 | Widgets touched from worker threads | **Partly fixed** (thread-safe `after()` everywhere; .NET dialog moved to Tk thread); `_detect_game_info` race remains |
| M4 | Folder walks / rmtree on the UI thread | Open |
| M5 | Sweep dismounts the user's own mounts | Open |
| M6 | Updater hash optional, no timeout | **Fixed** |
| M7 | Elevated app runs code from user-writable temp | **Partly fixed** (Backport.py private fresh folder); scripts/UFS2Tool still extracted to temp without a hash re-check |
| M8 | Duplicate pipelines | Open (design work) |
| M9 | Temp leftovers never recovered | **Partly fixed** (backport folders cleaned) |
| M10 | Console output decoded as `mbcs` | Open (needs Windows locale test) |
| L1–L6 | Low items | L2 fixed (shortcuts); others open |

## Issue audit (`ISSUE_AUDIT.md`) — status of defects still open there

| ID | Status |
|---|---|
| H1 finished .ffpfsc deleted when intermediate locked | **Fixed** |
| H2 unreadable image root reported as 0 files | **Fixed** |
| H3 PS1 counts skipped files | **Fixed** |
| M1 backport byte-patch fallback | **Fixed** (removed) |
| M2 stale Backport.py / sys.path | **Fixed** |
| M3 stale-mount sweep before build (#82) | Open (needs OSFMount on Windows) |
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
* **Not done:** building from `.exfat`/`.ffpkg` directly, non-ASCII paths
  (junctions), DLC (`prospero_ac`), SELF/UCP repair, DDS→PNG, update builds
  in the GUI (engine supports `ref_pkg`), extract/verify of existing `.pkg`
  in the GUI (engine has the argv helpers). Real-tool and console testing.

## Backport modernisation report

* **Done without the Backport GUI source:** removed the corrupting fallback;
  private, fresh engine folder; Target SDK ≤ game SDK; already-backported
  detection; other tools' fakelib backed up; free-space check; OS metadata
  files never treated as executables.
* **Blocked:** the Backport GUI source lives on the owner's PC (`D:\`), not on
  GitHub. Remote Control showed no shared folders during this session
  (`list_computer_folders` returned nothing), so its engine (import-table
  audit FW 4.03–13.60, confirmed fix families, audio check, ZIP hash
  verification, `right.sprx` replacement, PSSR 2 test) could not be merged.

## Next steps (resume here)

1. **When D:\ is reachable:** inventory the Backport GUI source; port its
   engine checks into `_abp_run` / Backport pipeline with tests; check the
   licence of any bundled asset (e.g. a standard `right.sprx`) before use.
2. On a real Windows PC with OSFMount: build one small game end to end
   (exFAT, FFPKG, PFS), verify, extract; confirm the rebuild/restore path
   and the close-during-build behaviour.
3. FPKG with the owner's Publishing Tools: one small game, then a
   multi-language game; record results in `docs/FPKG.md`.
4. Remaining audit items: M1, M4, M5, M10, issue-audit M3.
5. Release: only with the owner's approval (no public release or push to
   `main` without it). The CI exe artifact is a test build, not a release.
