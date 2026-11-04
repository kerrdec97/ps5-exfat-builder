# Backport: parity spec and C++ plan

Scope: the **Backports** area of the PS5 exFAT Image Builder Farewell Edition (Python/Tkinter, `APP_VERSION 4.1.0`, branch `claude/wizardly-sagan-mahxhj` based on `farewell-edition-dev`, HEAD `b298c2d`). It covers what the C++20/Qt 6 rebuild under `native/src/engines/backport/` must do.

| Sources | Abbrev. |
|---|---|
| `exfat_builder.py` (embedded engine is base64: `_BACKPORT_B64` = Backport.py, `_SRC_FSELF_B64` = make_fself.py, `_SRC_DECRYPT_B64` = decrypt_fself.py, `_SRC_PATCHER_B64` = ps5_sdk_version_patcher.py, `_SRC_ARCHIVE_B64` = Archive.py; decoded for this spec) | `E:` (line numbers), `BP:`, `MF:`, `DF:`, `PT:` |
| `ui/tab_backports.py`, `ui/panel_bp_auto.py`, `ui/backport_profiles.py`, `ui/build_ampr_index.py`, `core/backport_safety.py`, `tests/test_backport_*.py` | file names |
| `docs/farewell/STATUS.md` (includes the read-only D:\ audit summary), `ISSUE_AUDIT.md`, `DEPENDENCIES.md`, `CHANGELOG.md`, `THIRD_PARTY_NOTICES.md` | `ST`, `IA`, `DEP`, `CL`, `TPN` |
| PS5 Backport GUI public repo (docs only; no source): README, ROADMAP, CHANGELOG, docs/INSTALLATION, TROUBLESHOOTING, COMPATIBILITY, REPORTING_ISSUES, PRIVACY, THIRD_PARTY_NOTICES.txt, bug_report.yml, screenshots/ps5-backport-gui.png (0.2.0-beta) | `[R] [RM] [C] [I] [T] [CP] [RI] [P] [N] [BR] [S2]` |
| Owner's screenshot of Backport GUI 0.3.4 (`images/5.webp`) | `[S5]` |
| Upstreams cloned for licence checks: `Nazky/Auto-Backpork` (HEAD `dab6fad`, 2026-04-08), `drakmor/ampr_emu` (HEAD `cfa85df`, 2026-09-06) | `UP` |

**How this was checked:** I read the code statically. I then ran the decoded embedded engine on synthetic ELF/SELF files that I built from the format (no game data). The results are in Appendix A. I did not run the Tk app, use a PS5, or read the Backport GUI source on D:\. Anything I could not confirm is marked **UNVERIFIED**.

**Clean-room boundary.** Parts of the Python engine have no licence (§2.1). For those parts, this document gives only **facts**: byte offsets, magic values, constants and observable behaviour. The C++ engineer implements from this document and must not read or translate the unlicensed Python. GPL-3.0 parts may be ported with attribution.

---

## 0. Summary of decisions for the C++ port

| # | Decision | Why |
|---|---|---|
| D1 | The engine never writes into the game folder before the **Apply** phase. All unwrap, patch and sign work happens in a private staging folder. | On ELF-only dumps the Python engine SDK-patches the game's files in place and leaves `.bak` files (A.3). |
| D2 | Encrypted or compressed SELF entries are detected while scanning (props bit 1 / bit 3). The run is refused before anything changes unless the policy allows leaving them unchanged. | Python "decrypts" such files into garbage, and the patcher then reports `Invalid param magic 0x…`. That is the `right.sprx` failure class (A.4). |
| D3 | `fakelib/` and `decrypted/` are never SDK-patched or re-signed. Dump extras are carried over unchanged. | Python re-signs and SDK-patches a dump's own fakelib files (A.4). |
| D4 | A build with any per-file failure is **not output**: no backport ZIP and no apply. The status is Failed and the game is unchanged. | This matches Backport GUI ("refuses to output a build that fails" [R]). Python patches the game and writes the ZIPs anyway. |
| D5 | Apply to game folder is a per-job option written through a journal, with rollback after a crash. Default: **on**. This needs an owner decision: Backport GUI defaults to off, while this app's workflow is backport-then-build-image and the Python app always applied. | Parity plus safety. |
| D6 | Restore also removes the files the backport **added** (recorded in the manifest), such as `fakelib/`. | After a Python restore the fakelib stays, so the next run is refused as "already backported" (§2.15). |
| D7 | The game's own SDK comes from the `eboot.bin` process param. `param.json` is a fallback for display only. | Backport GUI 0.3.3 did the same [C]. Python falls back to `requiredSystemSoftwareVersion` (a firmware value, not the SDK). |
| D8 | The ZIP and manifest formats stay compatible with Python. The C++ app reads `BackportBackup/<TitleID>/manifest.json` schema 1 and writes schema 2, a superset. | Users keep their existing backups. |
| D9 | The SELF writer is a GPL-3.0 port with attribution. The SDK patcher, the pipeline and the NID patch are clean-room from §2. The AMPR index is a GPL-3.0 port of the **current** drakmor upstream. | Licences, §2.1. |
| D10 | Results "It worked" never deletes the originals ZIP automatically. | Python deletes the only copy of the originals without asking (§2.18). |

---

## 1. Backport GUI feature inventory (docs, changelog, screenshots)

Backport GUI is closed source and proprietary: "personal, non-commercial use", no derivative works, no reverse engineering [R][LICENSE.txt]. Version history: 0.2.0-beta [S2], early beta 18 Oct 2026 [C], 0.3.1, 0.3.2 (latest stable), 0.3.3 testing build (22 Oct 2026) [R], 0.3.4 [S5]. The D:\ checkout was `D:\fixing input\unity_input_gui`, branch `snapshot/20261021-f1-pro-v45`, HEAD `3cf4ffac` [ST].

### 1.1 Screens and controls

| Screen / area | Controls and content | Source |
|---|---|---|
| Header | Logo `D97`, "PS5 Backport GUI" + BETA badge, "by Deckerr97"; tabs **Games**, **Console Logs**; actions **Export logs**, **Report a problem**, **Settings**, **Support on Ko-fi** | [S5][S2] |
| Game Library card | Title + count ("5 games found" / "No games yet"); **Add folders**, **Rescan**, **Clear list**; **Select all**, **Select none**, **Scan locations (N)**; "Last scan hh:mm" or "Scanning…"; search box "Search title or PPSA…" | [S5][S2] |
| Library table | Columns: Select (checkbox), Cover, Game title (with folder path below), PPSA, SDK, Required FW, Status (for example "● Ready") | [S5][S2] |
| Save to card | **Backport ZIPs** path + browse; **Originals backup ZIPs** path + browse. Long paths show their end, with Windows backslashes [C] | [S5][C] |
| Options card | **Target SDK** dropdown with items like "SDK 4 · loads on FW 4.00+", and a hint "An SDK N build only loads on firmware N.00 and later."; checkbox **Apply to game folders** ("Copy the files into the game folders, after a backup."); checkbox **PS5 Pro fix** ("For a black screen or GPU hang on a PS5 Pro."); checkbox **PS5 Pro build (Pro FW 9.00+)** ("Pro-only ZIP: the game's own Pro checks, and its PSSR, answer Pro on every Pro firmware.") in 0.3.4 | [S5] |
| Tip box | "Black screen on FW 5.00+? Set Target SDK to 5 (or higher) and backport the unmodified game again… Keep the FW 4xx ZIP for FW 4.xx consoles. A right-click rebuild of an old ZIP keeps its FW 4.xx save fixes." | [S5] |
| Progress card | Bar; state text ("Idle"); counters **Games / Selected / Ready / Running / Done / Failed / partial** | [S5][S2] |
| Activity | Plan line "Next run: backport + originals ZIPs · game folders unchanged"; **Open output folder**, **Retry failed**, **Clear**; log lines `[hh:mm:ss] [INFO] …` | [S5] |
| Bottom right | **Switch AMPR / PlayGo…** (0.3.4), primary **Start Backport** | [S5] |
| Status bar | "● Ready" (left); "PS5 Backport GUI 0.3.4 · by Deckerr97 · backend 3e08eda5-914d90065d" (right) | [S5] |
| Row context menu | **Rebuild a ZIP with another SDK**; **Make a PS5 Pro PSSR 2 test from a backport** (accepts a backport ZIP or a backported folder without originals; a folder input gives a ZIP of the changed files only) | [R][C] |
| Settings | Subfolder depth (default 6); **Restore originals from a backup**; **Open logs folder**; list of applied backports | [I][T][R] |
| Console Logs tab | Console IP, port (default 9021), **Connect**, checkbox "Send klog + debug payloads on connect", live klog, auto-download of crash dumps into a ZIP, **Capture hang**, **Save & Zip logs**, **Open folder** (`Desktop\PS5 Logs`) | [R][RI] |
| Tooltips | Stay on screen; antialiased checkbox ticks | [C] |

### 1.2 Workflows

| Workflow | Behaviour | Source |
|---|---|---|
| Scan | Searches added folders and their subfolders (6 levels by default) for game folders, meaning a folder with `eboot.bin` at its root. Reads title, PPSA, version, SDK and required FW from `sce_sys/param.json`, or from the folder name. Scanning only reads files. The backport and originals folders are never scanned. Drag and drop of folders is supported. | [R][I][T] |
| Start Backport | Processes every ticked game in turn, with per-game status, run statistics, *Retry failed* and cancellation. | [R] |
| Compatibility baseline | Rewrites the SDK version of every title executable, fake-signs it, and adds the "fakelib provider set the pipeline uses for FW 4.xx" (bundled; "nothing to configure"). | [R][I] |
| Automatic fixes | Audits every executable against **firmware import tables FW 4.03–13.60**. Applies only "fix families" confirmed on hardware. Anything else is reported, not guessed. | [R] |
| Build verification | Re-audits the finished build, including an **audio check across firmware bands**. Verifies ZIP CRCs, signed-executable structure and the fakelib set. Refuses to output a failing build. Every ZIP is re-read and checked against recorded hashes. | [R][T] |
| Output | `FW 4xx <PPSA> <Title> (<version>) backport files.zip` in the backport folder. `<PPSA> <Title> (<version>) original files.zip` holds the untouched originals of **only the replaced files**. Nothing is overwritten. Per-run manifests record the SHA-256 of every file. Each folder gets a `_backport_details` subfolder with the loose files, reports and manifests. | [R][I] |
| Apply to game folders | Optional, off by default. Copies the backport into the game folder only after the originals are backed up and verified. Uses a write journal with automatic rollback on failure. An interrupted apply is offered for rollback at the next start. **Restore originals** is in Settings. | [R][T] |
| Target SDK | SDK 4 is the default and loads on FW 4.00+. SDK 5–13 are experimental. An SDK *N* build loads only on FW *N*.00+. The target can't be higher than the game's own SDK. SDK 5 fixed black screens and GPU hangs for some titles on newer firmware. SDK 6–13 are untested on hardware. | [R][I][RM] |
| Rebuild ZIP with another SDK | Right-click a game to rebuild its FW 4xx ZIP at the selected SDK. It keeps the FW 4.xx save fixes. | [R][S5] |
| PS5 Pro fix | Adds the fix set confirmed on one title (Assassin's Creed Black Flag Resynced). It is not confirmed on any other game. | [R] |
| PS5 Pro PSSR 2 test / Pro build | Pro FW 9.00+ only. Produces a Pro-only ZIP `… Pro PSSR2 test backport files.zip` (0.3.0 name; the 0.3.4 name is **UNVERIFIED**). The PSSR library uses the console's real Pro, app-info and title queries. A change to the bundled AGC library's Pro detection is tested. On Pro FW 9.x–12.x the game's requested PSSR version is restored. A requested model missing from the packaged files is refused. Games without PSSR get the normal ZIP. *Apply to game folders* warns about Pro-only files. | [C][S5] |
| Switch AMPR / PlayGo | Swaps a backport's `libSceAmpr.sprx` / `libScePlayGo.sprx` between the engine's providers (AMPR `82484486`, AMPR `0aadd31d`, PlayGo `a0f20f40`, as SHA-256 prefixes) and drakmor **ampr_emu 0.4.2.1** (`2feef6a2` / `f5fdb0ed`, GPL-3.0). The files are hash-pinned in `app/fakelib_variants/MANIFEST.json`. | [N][S5][IA #17 #27] |
| right.sprx | When `sce_sys/about/right.sprx` fails with `Invalid param magic 0x79A6EB9E`, it is replaced with "the standard copy". Other conversion failures may mean the dump is still encrypted. | [C] |
| Invalid param.json values | SDK/FW values such as 0.00, 70.00 or 92.00 (from repacked `param.json`) are ignored, and the SDK is read from `eboot.bin`. | [C] |
| Dump with another tool's fakelib | 0.3.3 builds them by replacing the dump's three libraries (#27). | [R][IA] |
| OS metadata | Leftover macOS/Windows metadata files no longer fail a build (0.3.3). | [R] |
| Already backported | "This game folder already contains backport files": the folder already has the backport fakelibs, and originals can't be captured from a patched folder. | [T] |
| Destination errors | Both folders must be set and must be different. Neither may be inside the other or inside a game folder, and both need enough free space. The message names the folder at fault. | [T] |
| Failure messages | "Target SDK N is higher than this game's own SDK"; "The SDK N build was refused" (for example, executables the signer can't rewrite); "No safe backport was produced"; "No backport is needed"; "Partial" (the build was made but the ZIP or apply step didn't finish). | [T] |
| Save data | 4.xx-only save fixes are no longer applied to newer SDKs (#5). The save-mount check counted only one of two identical requests and was fixed in 0.3.3 (#22–#24). | [IA] |
| Audio | 0.3.1 fixed no-audio cases (#10, #11, #13). 0.3.4 added a 2-byte eboot patch for CRI 7.1.4 → 7.1 (12 → 8 channels) (#28). | [IA] |
| Unity / Il2Cpp | MGS4 launch crash caused by the module-name case `Il2cppUserAssemblies.prx` vs `Il2CppUserAssemblies.prx`, fixed in 0.3.3 (#2). Other Unity handling is **UNVERIFIED**; the checkout folder name `unity_input_gui` suggests an origin in a Unity-input tool. | [IA][ST] |
| Console Logs | Sends the bundled klog, FTP and debug payloads (klogsrv and ftpsrv GPL-3.0, ps5debug-NG 1.3.2) to an ELF loader on port 9021. Streams the klog. Downloads crash dumps into `PS5_logs_<ip>_<time>_crash_<…>.zip`. Takes read-only hang snapshots. | [R][RI][N] |
| Export logs | `PS5BackportGUI_logs_<date>_<time>.zip` on the Desktop, then opens Explorer. Contains the activity log and, per run, the result, engine plan, build log, engine reports and error trace, plus the settings file and a system summary. No game files. Offered automatically after a failed run. | [RI] |
| Report a problem | Opens the GitHub bug form. Per [C] 0.3.0 it pre-fills the SDK/fakelib options, title, PPSA and version; per [P] it fills only the program version. The current behaviour is **UNVERIFIED**. Form fields: game_title, ppsa, game_version, firmware, console, format, storage, loader, tool_version, sdk, fakelib, happened, expected, steps, error, evidence. | [C][P][BR] |
| Storage | Settings and applied-backport list in `%APPDATA%\Deckerr97BackportGuiBeta\`. Logs `app_<date>.log` and `runs\<run>\` in `%LOCALAPPDATA%\Deckerr97BackportGuiBeta\logs\`. Engine unpacked to `%LOCALAPPDATA%\D97B\<version>` (about 160 MB). | [I] |
| Third-party (bundled) | Python 3.14.7, Tcl/Tk 9.0.4, **Capstone 5.0.9** (disassembly), **Unicorn 2.1.1** (CPU emulator, GPL-2.0), Pillow, PyInstaller, make_fself (GPL-3.0+), klogsrv/ftpsrv (GPL-3.0), ampr_emu (GPL-3.0). The engine's use of Capstone/Unicorn is **UNVERIFIED** (likely import/NID analysis). | [N] |

### 1.3 Facts from the read-only D:\ audit (from `ST`; no code was copied)

| Finding | Use for C++ |
|---|---|
| Import-table checks cover FW 4.03–13.60 but skip decode errors, merge gaps and trust seeded slots | Blocked: tables of undocumented origin |
| V2 registry, report-only patterns, firmware boundaries and planner families disagree | Nothing to port |
| Audio gate checks only `sceAudioOut2Set3DLatency`; the live parser keeps only the last 600 characters | Do not copy the pattern |
| AMPR `prtm` (JWE3 V32 kernel panic after about 1 minute) and `prtp` (fails on Planet Coaster 2 V3) | Not added; experimental if ever added |
| `packaged_ampr_sha` / `ampr_unmap_prt_fatal` leave ZIPs open | C++ uses RAII for all archive handles |
| Already-backported detection based on file names, no tests | Hash-based detection (§2.14) |
| Release-only `4e1d7250` rejects nested or undersized fake libraries | Ported independently (§3 S8) |
| Boundary UNKNOWN → `HARDWARE_MEASURED_FW13_20_PRO`; evidence: FW 4.50 Base rejection, Pro 13.20 | Record only |
| SDK 5–13 target pairs | Hardware proof reported for SDK 5 only |
| The checkout has no tracked project licence; a GPL-3.0 AMPR/PlayGo source ships without licence text or source offer; `right.sprx` (Sony-derived), PSSR and kernel files (game-derived) and the firmware import tables (undocumented origin) are present | All blocked (§4) |

---

## 2. Our embedded engine: algorithm spec

### 2.1 Components, provenance, licence

| Component | Where | Origin (verified) | Licence | C++ rule |
|---|---|---|---|---|
| `Backport.py` (`PS5ELFProcessor`: pipeline, libc NID patch, fakelib copy) | `E:82` `_BACKPORT_B64` | Byte-identical to `Nazky/Auto-Backpork` `Backport.py` @ `dab6fad` (SHA-256 `e88be878…`) | **None.** The upstream repo has no LICENSE file; checked in a shallow clone. All rights reserved. | **Do not port code.** Reimplement the behaviour from §2.2–2.10 |
| `make_fself.py` (ELF → fake-signed SELF) | `E:85` | john-tornblom, ps5-payload-dev/sdk `samples/install_app/make_fself.py`, modified by Nazky. Identical to the Auto-Backpork copy (`5d4397bc…`) | **GPL-3.0** (sdk `LICENSE`, verified in `DEP`) | Port allowed. Keep the attribution header and add a GPL notice in `THIRD_PARTY_NOTICES` |
| `decrypt_fself.py` (fake-signed SELF → ELF) | `E:84` | "Based on make_fself.py by john-tornblom / @NazkyYT" (`07bebda2…`) | GPL-3.0 as a derivative of make_fself. That Nazky intended GPL is **UNVERIFIED**, but a derivative must be GPL. | Port allowed with attribution. Prefer reimplementing it from §2.7 (it has bugs) |
| `ps5_sdk_version_patcher.py` | `E:86` | idlesauce gist `2ded24b7…` (`ps5_elf_sdk_downgrade.py`), modified by Nazky (`45b04cfc…`) | **None** (the gist has no licence; `DEP` and `TPN` keep [VERIFY] open) | **Do not port code.** Clean-room from §2.5–2.6. Offsets and SDK constants are facts. Legal review of using the constants is **UNVERIFIED**. |
| `Archive.py` | `E:83` | Nazky (adds a `getpass` shim) | None | Not needed: only Backport.py's CLI uses it |
| `ui/build_ampr_index.py` | file | Older revision of `drakmor/ampr_emu` `tools/build_ampr_index.py` (diffed; progress callbacks added) | **GPL-3.0** (repo LICENSE @ `cfa85df`) | Port the **current upstream** semantics with attribution (§2.17). Gap: our file has no attribution header, and `TPN` lists drakmor under "no code bundled". |
| `core/backport_safety.py`, `ui/backport_profiles.py`, `ui/audio_*.py`, Backports UI | files | Original to this project | Project licence | Port freely |

The C++ app is GPL-3.0-compatible, so GPL-3.0 ports are fine with notices plus corresponding source. Code with no licence (Backport.py, the patcher gist, Archive.py) must not be translated.

### 2.2 Run sequence (single game, "Apply Auto Backport", `E:16807-17313`)

| Step | Python behaviour | C++ (D1–D4) |
|---|---|---|
| 0 Preflight | Safety checks S3–S12 (§3), then a confirm dialog | Same, as `BackportPlanner::plan()`. Batch runs skip instead of asking. |
| 1 Temp | `mkdtemp('exfat_builder_abp_')` in the temp dir → `originals/`, `patched/` | `TempDir` from `JobContext.tempDir` → `originals/`, `unwrapped/`, `signed/`, `stage/` |
| 2 File list | Records every relative path in the game, lower-cased, skipping dirs named `decrypted`. Used later as "existed before". | Same, from `FileTree` |
| 3 Snapshot | Copies every executable from the scanner (§2.3) to `originals/<rel>`. A copy error stops the run. | Same. Also hashes each file (sha256 before). |
| 4 Fakelib overlay | When a fakelib folder is set: for every file of the set (recursive), `game/fakelib/<rel>` → `originals/fakelib/<rel>` if it exists, then copy the set file into `game/fakelib/<rel>`. **Writes into the game before the engine runs.** | Staged into `stage/fakelib/`. The game is untouched until Apply. |
| 5 Engine | `decrypt_and_sign_pipeline(input=game, output=patched, sdk_pair=N, paid=0x3100000000000002, ptype=1, fakelib_source, create_backup=True, overwrite=True, apply_libc_patch=True, auto_revert_for_high_sdk=True)`; output is streamed to the log, with a heartbeat every 15 s | §2.3–2.10 on copies |
| 6 Copy back | Walks `patched/` (skipping `decrypted`). Strips a leading component equal to the game folder name. If the target exists and was in the step 2 list and no backup exists yet, it is copied to `originals/<rel>`. Then the file is copied over the game file. | Apply phase with a journal (§2.12), only when D4 passes and the option is on |
| 7 `.bak` cleanup | `remove_new_baks(game, before)` deletes `<exec>.bak` files that were not there before | Nothing to clean: the engine never writes into the game |
| 8 Count | `failed = decrypt.failed + signing.failed + downgrade.failed` (a file whose patch fails is counted **twice**: downgrade and "Skipped due to downgrade failure") | Per-file results; each file counted once |
| 9 ZIPs | §2.13; the originals ZIP is required | Same names. Build verification before the ZIPs (§6.4). |
| 10 Manifest | §2.14 | Schema 2 |
| 11 Cleanup | The temp folder is deleted only after the originals ZIP is written and read back. Otherwise the log says where the originals are. | Same (`TempDir::keep()`) |
| 12 Result | `ok = originals_zip_ok and (no backport folder or backport_zip_ok) and failed == 0` | Succeeded / Partial / Failed (§6.3) |

"Backport All" (`E:22403-22840`, also used by the Games-tab queue and the single-card action) runs the same sequence per game. Differences: it uses `_abp_all_preflight` with no dialogs and skips the game on doubt; it does **no free-space check**; it applies the name-case setting to the ZIP prefix; it asks for the Target SDK once (4–11, clamped to 1–10) and stores it as `abp_last_sdk`.

### 2.3 File discovery and classification

| Rule | App scanner `_abp_scan_executables` (`E:15324-15379`) | Engine walk (`BP: decrypt_and_sign_pipeline`) |
|---|---|---|
| Dirs skipped | Names `decrypted` and `fakelib` (any case); `__MACOSX`, `.Trashes`, `.fseventsd`, `.Spotlight-V100`, `.TemporaryItems`, `System Volume Information`, `$RECYCLE.BIN` | Only dirs named `decrypted` (any case). **`fakelib/` is processed.** |
| Files skipped | Names starting `._`; `.ds_store`, `thumbs.db`, `desktop.ini`, `ehthumbs.db`, `.localized`; names ending `.bak` | Names ending `.bak` |
| ELF | First 4 bytes `7F 45 4C 46` | Same |
| SELF | First 4 bytes `4F 15 3D 1D` (LE `0x1D3D154F`, "PS4"/fake-signed) or `54 14 F5 EE` (LE `0xEEF51454`, "PS5") | Same |
| Unreadable header | If fewer than 4 bytes can be read, the name decides: `eboot.bin`, or ext `.prx .sprx .self .elf` | Read error → not executable |
| Readable non-magic | Not executable, whatever the name | "other" |

C++: **one** scanner, used for both preflight and processing. Exclusions are the union of both columns plus `sce_sys/` files without ELF/SELF magic. Classification (§2.7) also marks encrypted and compressed SELFs. `fakelib/` and `decrypted/` are listed but never processed (D3). Files named `*.bak` are reported as leftovers.

### 2.4 ELF64 structures used

| ELF header field | Off | Size | Required by signer (`MF`) | Read by patcher |
|---|---|---|---|---|
| `e_ident[0..3]` | 0x00 | 4 | `7F 'E' 'L' 'F'` | yes (magic) |
| `EI_CLASS` | 0x04 | 1 | 2 (64-bit) | no |
| `EI_DATA` | 0x05 | 1 | 1 (LE) | no |
| `EI_VERSION`, `EI_OSABI`, `EI_ABIVERSION`, pad, `EI_NIDENT` byte | 0x06–0x0F | 10 | stored as-is | no |
| `e_type` | 0x10 | 2 | 2 `ET_EXEC`, `0xFE00` SCE_EXEC, `0xFE10` SCE_EXEC_ASLR, `0xFE18` SCE_DYNAMIC (PRX) | no |
| `e_machine` | 0x12 | 2 | `0x3E` (x86-64) | no |
| `e_version` | 0x14 | 4 | 1 | no |
| `e_entry` | 0x18 | 8 | — | no |
| `e_phoff` | 0x20 | 8 | — | **yes** |
| `e_shoff` | 0x28 | 8 | — | no |
| `e_flags` | 0x30 | 4 | — | no |
| `e_ehsize` | 0x34 | 2 | — | no |
| `e_phentsize` | 0x36 | 2 | must be `0x38` | no (assumes 0x38) |
| `e_phnum` | 0x38 | 2 | — | **yes** |
| `e_shentsize` | 0x3A | 2 | 0 or `0x40` | no |
| `e_shnum` | 0x3C | 2 | forced to **0** in the SELF copy | no |
| `e_shstrndx` | 0x3E | 2 | — | no |

| Program header (0x38 bytes) | Off | Size |
|---|---|---|
| `p_type` | 0x00 | 4 |
| `p_flags` | 0x04 | 4 |
| `p_offset` | 0x08 | 8 |
| `p_vaddr` | 0x10 | 8 |
| `p_paddr` | 0x18 | 8 |
| `p_filesz` | 0x20 | 8 |
| `p_memsz` | 0x28 | 8 |
| `p_align` | 0x30 | 8 |

| `p_type` | Value | Becomes a SELF segment | Notes |
|---|---|---|---|
| `PT_LOAD` | `0x1` | yes | |
| `PT_DYNAMIC` | `0x2` | no | |
| `PT_INTERP` | `0x3` | no | |
| `PT_TLS` | `0x7` | no | |
| `PT_SCE_RELA` | `0x60000000` | no | |
| `PT_SCE_DYNLIBDATA` | `0x61000000` | yes | NID strings live here (§2.9) |
| `PT_SCE_PROCPARAM` | `0x61000001` | no | Patched (§2.5). In real executables it lies inside a `PT_LOAD`, so the patch reaches the SELF through that load segment. That this holds in all real executables is **UNVERIFIED**; it holds in Python's model. |
| `PT_SCE_MODULE_PARAM` | `0x61000002` | no | Patched (§2.5) |
| `PT_SCE_RELRO` | `0x61000010` | yes | |
| `PT_SCE_COMMENT` | `0x6FFFFF00` | yes | |
| `PT_SCE_VERSION` | `0x6FFFFF01` | no | Its bytes are appended after the SELF's `file_size` |
| `PT_GNU_EH_FRAME` / `PT_GNU_STACK` | `0x6474E550` / `0x6474E551` | no | |

### 2.5 SDK version patch (process/module param), clean-room facts

Param struct at `p_offset` of a `PT_SCE_PROCPARAM` / `PT_SCE_MODULE_PARAM` segment:

| Off | Size | Field | Value |
|---|---|---|---|
| 0x00 | 8 | struct size (u64; the patcher reads the low u32) | Process param observed as `0x60` in real eboots (comment in `E:16184`). Other values are **UNVERIFIED**. |
| 0x08 | 4 | magic (u32 LE) | process `0x4942524F` (bytes `4F 52 42 49` = "ORBI"); module `0x3C13F4BF` |
| 0x0C | 4 | entry count / unknown | not touched |
| 0x10 | 4 | **PS4 SDK version** (u32 LE) | rewritten |
| 0x14 | 4 | **PS5 SDK version** (u32 LE) | rewritten |

Algorithm (Python behaviour; C++ keeps the outcomes and messages but works on a staging copy):

| # | Step | Outcome / message |
|---|---|---|
| 1 | Bytes 0..3 ≠ ELF magic | SELF magic: fail "Skipping signed file '…' (expected unsigned ELF)"; otherwise fail "Not an ELF file" |
| 2 | (Python only) create `<file>.bak` unless one exists, **before** any further validation | A `.bak` exists even for files that then fail (A.4). C++: no `.bak`; the original stays in `originals/`. |
| 3 | Read `e_phoff` (u64 @0x20) and `e_phnum` (u16 @0x38); entries are 0x38 apart | |
| 4 | For each phdr with `p_type` ∈ {`0x61000001`, `0x61000002`}: `s = u32 @ p_offset` | |
| 5 | `s == 0` and module param | **Stop and succeed**: "Module file has no param, skipping" (remaining phdrs are not checked) |
| 6 | `s < 0x18` | fail "Unexpected param struct size 0x%X" |
| 7 | magic @+8 ≠ expected for the type | fail "Invalid param magic 0x%08X" (the `right.sprx` error) |
| 8 | Write the pair's PS5 value to +0x14 and the PS4 value to +0x10; continue with the next phdr (**every** matching segment is patched) | log "PS5 SDK version 0x… -> 0x…, PS4 version 0x… -> 0x…" |
| 9 | After the loop, nothing patched | fail "No process or module param segment found" |
| — | Out-of-range read | fail "Error patching '…': …". Python can leave a file **partially patched** when a later segment fails. C++ patches a copy and discards it on failure. |

The C++ must also check that `p_offset + 0x18 ≤ file size` and `p_offset + 0x18 ≤ p_offset + p_filesz` (Python does not), and must report the old SDK per file.

### 2.6 SDK pair table and firmware

| Pair N | PS5 SDK (u32 @+0x14) | Decoded (BCD major.minor, low 16 bits) | PS4 compat SDK (u32 @+0x10) | Decoded | Loads on firmware (per [R][I]) | Hardware evidence |
|---|---|---|---|---|---|---|
| 1 | `0x01000050` | 1.00 / `0x0050` | `0x07590001` | 7.59 | FW 1.00+ | **UNVERIFIED** |
| 2 | `0x02000009` | 2.00 / `0x0009` | `0x08050001` | 8.05 | FW 2.00+ | **UNVERIFIED** |
| 3 | `0x03000027` | 3.00 / `0x0027` | `0x08540001` | 8.54 | FW 3.00+ | **UNVERIFIED** |
| 4 | `0x04000031` | 4.00 / `0x0031` | `0x09040001` | 9.04 | FW 4.00+ (default; jailbreak targets 4.03–4.51) | Baseline of both tools |
| 5 | `0x05000033` | 5.00 / `0x0033` | `0x09590001` | 9.59 | FW 5.00+ | Reported for some titles [R][ST] |
| 6 | `0x06000038` | 6.00 / `0x0038` | `0x10090001` | 10.09 | FW 6.00+ | **UNVERIFIED** |
| 7 | `0x07000038` | 7.00 / `0x0038` | `0x10590001` | 10.59 | FW 7.00+ | **UNVERIFIED** (BGUI issue reports on 7.00) |
| 8 | `0x08000041` | 8.00 / `0x0041` | `0x11090001` | 11.09 | FW 8.00+ | **UNVERIFIED** |
| 9 | `0x09000040` | 9.00 / `0x0040` | `0x11590001` | 11.59 | FW 9.00+ | **UNVERIFIED** |
| 10 | `0x10000040` | 10.00 / `0x0040` | `0x12090001` | 12.09 | FW 10.00+ | **UNVERIFIED** |
| 11–13 | — | — | — | — | FW 11/12/13.00+ in Backport GUI | **Blocked**: no documented values. The pattern (PS4 +0.50 per pair) is an extrapolation and must not be used. |

Decoding: the major version is the top byte read as BCD (`(b>>4)*10 + (b&0xF)`). `0x10` is SDK 10, not 16. `E:16184 _batchsdk_read_eboot_sdk` treats `0x10` as 16 unless the exact pair value matches; the C++ must use BCD. The "SDK 6 libc/fakelib boundary": Python warns when the current and target SDK are on different sides of 6 (`_abp_crosses_sdk6`, `E:15315`), and the libc NID patch changes direction at 6 (§2.9).

### 2.7 SELF reader ("decrypt" = unwrap of an unencrypted fake-signed SELF)

SELF header (shared by reader and writer):

| Off | Size | Field |
|---|---|---|
| 0x00 | 4 | magic `4F 15 3D 1D` (fake/PS4) or `54 14 F5 EE` (PS5) |
| 0x04 | 1 | version |
| 0x05 | 1 | mode |
| 0x06 | 1 | endian |
| 0x07 | 1 | attributes |
| 0x08 | 4 | key_type (u32) |
| 0x0C | 2 | header_size (u16) |
| 0x0E | 2 | meta_size (u16) |
| 0x10 | 8 | file_size (u64) |
| 0x18 | 2 | num_entries (u16) |
| 0x1A | 2 | flags (u16) |
| 0x1C | 4 | padding |
| 0x20 | 0x20 × n | entry table |

| Entry (0x20 bytes) | Off | Size |
|---|---|---|
| props (u64) | 0x00 | 8 |
| offset in SELF (u64) | 0x08 | 8 |
| filesz (u64) | 0x10 | 8 |
| memsz (u64) | 0x18 | 8 |

| props bit(s) | Meaning |
|---|---|
| 0 | order |
| 1 | **encrypted** |
| 2 | signed |
| 3 | **compressed** |
| 8–10 | window bits |
| 11 | **has_blocks** (data entry) |
| 12–15 | block size code: `1 << (12 + code)` |
| 16 | has_digests (meta entry) |
| 17 | has_extents |
| 20–35 | segment_index (16 bits). Bit 20 doubles as `has_meta_segment` in the Python classes. |

Unwrap algorithm. Columns: what Python does (`DF`) and what C++ must do.

| # | Python | C++ |
|---|---|---|
| 1 | Accepts either magic | Same. Validate `0x20 + 0x20·n ≤ size`. |
| 2 | ELF header at `E = align16(0x20 + 0x20·n)`. Reads 64 bytes, **no validation**. Phdrs at `E + e_phoff`. | Validate with the §2.4 rules (as for signing) |
| 3 | Extended info at `align16(E + e_ehsize + e_phnum·e_phentsize)`; read only for display | Parse; report the paid/ptype |
| 4 | **Ignores the encrypted and compressed bits** | Any data entry with bit 1 or bit 3 → `SelfEncrypted` / `SelfCompressed` ("the dump may still be encrypted"), unless the policy says leave unchanged (S31) |
| 5 | Output = `e_ehsize` bytes of the header, then the phdr table **written straight after it** (correct only if `e_phoff == e_ehsize`) | Write the header at 0 and the phdr table at `e_phoff` |
| 6 | Data entries = entries with bit 11. Sorted by segment_index; index ≥ `e_phnum` is skipped. Each is written at `p_offset` of its phdr (zero padding before it, seek back on overlap), then padded to `p_offset + p_filesz`. | Same, plus bounds checks (`offset + filesz ≤ SELF size`, `filesz ≤ p_filesz`) |
| 7 | For each `PT_SCE_VERSION` phdr with `p_filesz > 0` and no data entry: the **last `p_filesz` bytes of the SELF file** are written at `p_offset` | Same, but only if `size − file_size == p_filesz`; otherwise a warning and the segment is skipped |
| 8 | No section headers. Output ends at the end of the last written segment. | Same (document it). The output is an intermediate, never shipped. |

Round trip: Python unwrap(writer(ELF)) gives the identical ELF for F1 and F2 (Appendix A), provided `e_shnum == 0`, `e_phoff == 0x40` and no trailing bytes after the last segment.

### 2.8 SELF writer (fake signing), GPL-3.0 port of make_fself

Inputs: the SDK-patched ELF (read with section headers ignored, so `e_shnum` becomes 0), `paid = 0x3100000000000002`, `ptype = 1` (FAKE), `app_version = 0`, `fw_version = 0`, `auth_info = none`. These are the only values Python uses. The CLI offers others (system paid `0x3200000000000001`, NPDRM `0x3300000000000003`; ptypes 4/5/8/9/0xC/0xE/0xF), and the GUI never uses them.

Let `Q` = the phdrs with type ∈ {LOAD, SCE_DYNLIBDATA, SCE_RELRO, SCE_COMMENT}, in phdr order, and `n = 2·|Q|`. Let `E = 0x20 + 0x20·n` and `H = align16(max(e_ehsize, e_phoff + e_phentsize·e_phnum))`.

| Offset | Size | Field | Value written |
|---|---|---|---|
| 0x00 | 4 | magic | `4F 15 3D 1D` |
| 0x04 | 4 | version, mode, endian, attributes | `00 01 01 12` |
| 0x08 | 4 | key_type | `0x00000101` |
| 0x0C | 2 | header_size | `E + H + 0x40 + 0x30` |
| 0x0E | 2 | meta_size | `0x50·n + 0x50 + 0x100` |
| 0x10 | 8 | file_size | end of the last segment, aligned 16 (the version tail is **not** counted) |
| 0x18 | 2 | num_entries | `n` |
| 0x1A | 2 | flags | `0x0022` (`0x2 \| (2 << 4)`) |
| 0x1C | 4 | pad | 0 |
| 0x20 | 0x20·n | entries | see below |
| `E` | 0x40 | ELF header copy | input header with `e_shnum = 0` (shoff, shentsize, shstrndx kept) |
| `E + e_phoff` | `0x38·e_phnum` | phdr table copy | verbatim |
| `E + H` | 0x20 | ext info | u64 paid, u64 ptype, u64 app_version, u64 fw_version |
| `E + H + 0x20` | 0x20 | digest | SHA-256 of the **whole input ELF file** |
| `E + H + 0x40` | 0x30 | NPDRM control block | u16 type = 3, 14 zero bytes, content_id 19 zero bytes, random_pad 13 zero bytes |
| `header_size` | `0x50·n` | meta blocks | zero |
| `+0x50·n` | 0x50 | meta footer | zero except u32 @+0x30 = `0x00010000` |
| `+0x50` | 0x100 | signature | zero |
| `header_size + meta_size` | … | segment data | see below |
| `file_size` | `p_filesz` of the **last** `PT_SCE_VERSION` phdr | version tail | its ELF bytes (written even if empty) |

Segment data placement: the cursor starts at `header_size + meta_size`. For the k-th phdr `i` in `Q`:

| Entry | Index | props | offset | filesz = memsz | data |
|---|---|---|---|---|---|
| meta | 2k | `0x10004 \| ((2k+1) << 20)` (signed, has_digests, segment_index = index of its data entry) | cursor | `0x20 · ceil(p_filesz / 0x4000)` | zero bytes (empty digests) |
| data | 2k+1 | `0x2804 \| (i << 20)` (signed, has_blocks, block code 2 = 0x4000, segment_index = phdr index) | `align16` after meta | `p_filesz` | ELF bytes `[p_offset, p_offset + p_filesz)` |

After each entry, `cursor = align16(cursor + filesz)`. Gaps are zero-filled. The file is extended to `file_size`, then the version tail is appended.

Signing fails (the file is not produced) when the ELF header rules in §2.4 are not met. C++ adds: truncated segment (`p_offset + p_filesz > size`) → `CorruptData`; `header_size > 0xFFFF` → `Unsupported`.

Worked example (F1, pair 4; Appendix A): n = 6, header_size `0x2B0`, meta_size `0x330`. Entries at `0x5E0` (meta, props `0x110004`), `0x620` (data `0x2804`, 0x4800 bytes), `0x4E20` (`0x310004`), `0x4E40` (`0x102804`), `0x4F40` (`0x510004`), `0x4F60` (`0x302804`). file_size `0x5060`, total length `0x5080`. First 0x40 bytes:
`4f153d1d00010112 01010000b0023003 6050000000000000 0600220000000000 0400110000000000 e005000000000000 4000000000000000 4000000000000000`.

### 2.9 libc NID patch (Backport.py, no licence; data facts only)

| Item | Value |
|---|---|
| Pattern A ("original") | ASCII `4h6F1LLbTiw#A#B` |
| Pattern B ("replacement") | ASCII `IWIBBdTHit4#A#B` |
| Files | Every signed output SELF (any name), plus any file named `libc.prx`, under the engine output folder (not `decrypted/`, before fakelib is copied) |
| Target SDK ≤ 6 (apply) | If the file contains A: when it also contains B → "already_patched" and nothing changes; otherwise **every** A is replaced by B |
| Target SDK > 6 (revert) | If the file contains B and not A → every B is replaced by A |
| When | **After** signing, so the SELF's ex-info digest no longer matches the patched content. That fake-signed loaders do not check this digest is **UNVERIFIED**. |
| Meaning | The strings are SCE import NIDs (`<nid>#<lib>#<module>`) in `PT_SCE_DYNLIBDATA`. Which libc functions they name, and why SDK 6 is the boundary, is **UNVERIFIED**. The revert could also rewrite a newer-SDK game that legitimately imports B (**UNVERIFIED** risk). |
| Not used | SDK Fixer, Batch SDK Retag and `sign_file()` disable it |

C++: data-driven table `NidSubstitution{from, to, applyWhenTargetSdkAtMost=6, revertAbove=true}`. Keep Python's post-sign order for byte parity (golden in Appendix A). Setting `backport.nidPatchMode` (§5).

### 2.10 Fakelib rules

| Rule | Python | C++ |
|---|---|---|
| Source | Folder only. The "Select file" mode exists in the UI but `validate_fakelib_source` rejects anything that isn't a folder. Default: None. | Folder only |
| Validation | S8 (§3) | Same |
| GUI overlay (step 4 of §2.2) | Copies each file of the set to `game/fakelib/<rel>`, backing up an existing file to `originals/fakelib/<rel>` (Backport All: only if no backup exists yet) | Staged; applied in the Apply phase; backup recorded in the journal and the originals ZIP |
| Engine copy | If the source doesn't exist: success "(skipping)"; if it isn't a dir: fail; otherwise `rmtree(out/fakelib)` then `copytree(src, out/fakelib)` | `stage/fakelib/` = the source set |
| Nested eboots | For every `eboot.bin` anywhere under the output (including `decrypted/`!) whose folder isn't the output root: `rmtree(<dir>/fakelib)`, `copytree(src, <dir>/fakelib)` | For each `eboot.bin` in a sub-folder of the **game**: stage `<dir>/fakelib/` |
| Dump's own fakelib | Processed by the engine (unwrap → SDK patch → re-sign). Files in the set are then replaced by the set's copy; **extras such as `libSceAmpr.sprx` or `libSceAppContent.sprx` stay re-signed** and are copied back with a backup | Extras are carried over unchanged, with a warning listing them (AMPR/PlayGo/AppContent named, #17/#27). Replacing libSceAmpr / libScePlayGo is "Switch AMPR/PlayGo" (#34, `backport.amprPlaygoFolder`). |
| State check | `backport_fakelib_state(game, src)`: `none` (no or empty `game/fakelib`), `same` (every set file exists with the same size and SHA-256), `other` | Same |

### 2.11 `.bak` handling

| Producer | Where | Fate |
|---|---|---|
| SDK patcher (`create_backup=True` in all GUI flows except Batch Retag) | Next to each patched ELF: in `out/decrypted/` (SELF or mixed dumps) or **in the game** (ELF-only dumps) | `decrypted/` is excluded from ZIPs and copy-back. In the game, removed by `remove_new_baks`, which deletes only `<name>.<.bin/.elf/.self/.sprx/.prx>.bak` that were not there before (outside `decrypted/`). |
| libc patch | `create_backup=False` in the pipeline | — |
| SDK Fixer, overwrite mode | `<file>.bak` next to each changed executable, kept on purpose; removed again if nothing changed | Kept |
| Scanner | `.bak` is never treated as an executable | — |

C++: no `.bak` files are written anywhere (originals live in staging and in the ZIP). Pre-existing `.bak` files are left alone and reported.

### 2.12 Apply to game folder (copy-back)

Python always applies. There is no journal and no rollback; a crash midway leaves a mixed folder, and the originals are only in temp until the ZIP is written.

C++ Apply phase. It runs only when D4 passes and the originals ZIP has been written and verified.

| Step | Detail |
|---|---|
| 1 | Write the journal `<dataDir>/backport/journals/<jobId>.json`: game, list of `{rel, action: replace or add, sha256Before, sha256After, backupSource: zip:<arcname> or staging:<path>}`, state `pending` |
| 2 | For each entry: copy `stage/<rel>` → `game/<rel>.fwpart`, then `MoveFileEx(REPLACE_EXISTING)`; mark the entry done; fsync the journal every N entries |
| 3 | Verify each applied file's SHA-256 equals `sha256After` |
| 4 | On error or cancel: roll back done entries in reverse (replace → restore from staging originals; add → delete), verify, then mark the journal `rolledBack` |
| 5 | At start-up, a journal still `pending` → offer "Roll back the interrupted backport of <game>" (Backport GUI parity [T]) |
| 6 | Success: journal `done`, kept for 30 days for diagnostics |

The image-level "Apply backport" in File Manager (`ui/tab_files.py:673`) and the ffpkg editor (`ui/tab_ffpkg_edit.py:2019`) overlays a backport **folder** onto a mounted image or `.ffpkg` and zips the replaced files to `<image>.backup-<ts>.zip`. It belongs to those modules. The backport engine only provides the backport ZIP and folder.

### 2.13 ZIP naming, layout and writing

| Item | Rule |
|---|---|
| Prefix | `_sfo_zip_prefix` (`E:17473`): reads `TITLE_ID`, `TITLE`, `APP_VER` from `param.json` (keys `titleId`, `localizedParameters.en-US.titleName`, `titleName`, `contentVersion`, `appVersion`, …; also checks the parent folder), else from `param.sfo`. Title: removes `\ / : * ? " < > \| ™ ® ©`. Fallbacks from the folder name: PPSA regex `(PPSA\|CUSA\|PPSB)\d+`, version `\d{2}\.\d{3}\.\d{3}`, title stripped of IDs, versions, `[…]`, `(…)`, `Nxx`, `-app`, `.partN`. Result: `PPSA Title (ver)`, `PPSA Title`, `PPSA`, folder name or `Unknown Game`. Backport All also applies the name-case setting (`_apply_name_case`). |
| Originals ZIP | `<originalsDir>/<prefix> original files.zip` |
| Backport ZIP | `<backportDir>/FW <N>xx <prefix> backport files.zip`, where N = target SDK (e.g. `FW 4xx`, `FW 10xx`) |
| No overwrite | `unique_path`: `<stem> (2).zip`, `(3)`, … |
| Writing | `<zip>.part` → ZIP_DEFLATED with ZIP64 allowed → re-open → `testzip()` (CRC of every member) → member count must equal the distinct names → `os.replace` to the final name. On any error the `.part` is deleted and the error raised. Arcnames use `/` and are deduplicated case-insensitively (first one wins). |
| Originals ZIP contents | (1) the `originals/` staging tree minus any `decrypted/` dir: every executable from the snapshot, `fakelib/<rel>` of another tool's replaced files, and any original file the copy-back overwrote; (2) `sce_sys/param.json` ← the first `param.json` found (`sce_sys/param.json`, `param.json`, then a walk ≤ 3 levels deep), **always stored under that arcname**; (3) every non-empty file under any `decrypted` dir ≤ 4 levels deep, with its arcname relative to the game root |
| Backport ZIP contents | The `patched/` tree minus `decrypted/`: signed executables at their relative paths, `fakelib/` (the set), `<subdir>/fakelib/` for nested eboots, and the dump's re-signed fakelib extras (§2.10) |
| Backport GUI difference | Its originals ZIP holds "the untouched originals of **only the replaced files**" [R]. Ours is a superset: all executables plus param.json plus the dump's `decrypted/`. C++ keeps the superset (restore and re-patch need it). Instead of a `_backport_details/` folder, each run writes a report (JSON + HTML) to `BackportBackup/<TitleID>/…/reports/` (matrix #24). |
| Retagged ZIP (Batch SDK Retag) | Same name with the first `FW\s*\d+\.?xx` replaced by `FW <tgt>xx`, or that tag prepended; `_retagged_` prefix if it would equal the source path; written as `.part`, then renamed |

C++ entry order: sorted by arcname, for deterministic ZIPs.

### 2.14 Manifest and already-backported detection

Path: `<originalsZipFolder>/BackportBackup/<TitleID>/manifest.json`. TitleID comes from `get_game_info` (param.sfo → pfs-version.dat → param.json), falls back to the folder basename, then `UNKNOWN`, and is sanitised to `[A-Za-z0-9_-]`. It is written atomically (temp file in the same dir, then `os.replace`) after each run whose originals ZIP succeeded.

```json
{
  "schema_version": 1,
  "title_id": "PPSA01234",
  "title": "Game",
  "game_folder": "D:\\Games\\PPSA01234-app",
  "tool_version": "4.1.0",
  "created_at": "2026-11-04 05:00:00",
  "updated_at": "2026-11-04 05:00:00",
  "current_target": "4xx",
  "originals": {
    "zip": "<path relative to BackportBackup/<TitleID> if inside it, else absolute>",
    "zip_abs": "E:\\Zips\\PPSA01234 Game (01.000.000) original files.zip",
    "zip_sha256": "<hex>",
    "files": { "eboot.bin": "<sha256>", "sce_sys/param.json": "<sha256>", "decrypted/eboot.bin": "<sha256>" },
    "snapshot_at": "2026-11-04 05:00:00"
  },
  "targets": {
    "4xx": { "built_at": "…", "tool_version": "4.1.0", "post_hashes": { "eboot.bin": "<sha256>", "fakelib/libSceAgc.sprx": "<sha256>" } }
  },
  "history": [ { "action": "apply_target", "target": "4xx", "timestamp": "…", "tool_version": "4.1.0" } ]
}
```

| Field | Producer |
|---|---|
| `originals.files` | SHA-256 of each member, read back from the written originals ZIP |
| `targets.<N>xx.post_hashes` | `tree_hashes(patched, strip_first=<game folder name>)`: files with ext `.bin .elf .self .sprx .prx` (metadata excluded) outside `decrypted/` |
| `<N>xx` | `normalize_fw(sdk)` = leading digits + `xx` |

Verdict `backported_by_manifest(game, manifest)`:

| Step | Rule |
|---|---|
| 1 | No manifest → `unknown` |
| 2 | `patched` = union of all targets' `post_hashes` (keys lower-cased) |
| 3 | `originals` = `originals.files` whose key ends with an executable ext and does not start with `decrypted/` or `fakelib/` |
| 4 | Any `patched` key outside `fakelib/` whose current game file has that hash → **`patched`** (+ list) |
| 5 | Otherwise compare `originals` to the current files (missing files are skipped): any difference → **`differs`** (+ list); else any match → **`original`**; else **`unknown`** |
| Use | Single run: `patched` → refuse, `differs` → ask. Backport All: both → skip. |

C++ schema 2 adds `engine` (name, version, SDK table hash), `added_files` (paths created by Apply, for D6), `applied` (bool), `notes`, `files_after` with sizes, and `journal` id. It reads schema 1 unchanged. Keys are compared case-insensitively on Windows.

### 2.15 Restore

| Flow | Python behaviour | Gaps → C++ |
|---|---|---|
| Restore originals (single, `E:14912`) | The user picks a ZIP and confirms. `restore_zip(zip, game)`: for every non-directory member, `safe_member_target` → refuses empty names, a leading `/`, a drive letter or `:` in the first component, and any `..` component; `.` and empty parts are dropped; the result must stay inside the root. Then it creates parent dirs and overwrites. Returns `(restored, errors)`. | Does not remove added files (`fakelib/`, nested `fakelib/`), so a re-run is refused as `same`. Does not check the ZIP hash against the manifest. Does not update `current_target`. C++: verify `zip_sha256` (warn if no manifest), restore, delete `added_files`, verify against `originals.files`, set `current_target = null`. |
| Restore multiple (`E:15006`) | Candidates come from the Backports scan and the Library scan, de-duplicated. The originals folder comes from the form, then settings, then a picker. Each game is matched by TITLE_ID (param, else the folder-name regex): ZIP name contains `original files` and the ID, newest mtime wins. Checkbox list, worker thread; a partial restore counts as a failure. | Same, plus the manifest path match first |
| Profile manager (`ui/backport_profiles.py`: `verify_backup`, `verify_clean`, `restore_originals`, `preview_changes` build/swap/remove) | The "switcher" sub-tab that uses it is **not registered** (unreachable). Its `restore_originals` has **no zip-slip check**. | Fold into the C++ restore service; Swap = restore + new build |

### 2.16 Game metadata and SDK detection

| Value | Python source | C++ |
|---|---|---|
| Title, ID, version | `param.json` (`sce_sys/` or root, also the parent folder), then `param.sfo` (PSF: key table @8, data table @12, count @16, 16-byte index entries) | Shared `GameMetadata` reader (Library module) |
| Game SDK | `_read_sdk_from_folder`: `param.sfo` `SYSTEM_VER`/`SDK_VER`/`SYSTEM_VERSION` (major = byte at shift 24, else 16, accepted when 1..30), then `param.json` `sdkVersion`, `sdk_version`, `SDK_VER`, **`requiredSystemSoftwareVersion`**, `systemVersion`, `SYSTEM_VER`, parsed by `_parse_ps5_version_field` (hex string read as BCD digits: `0x1270…` = 12.70; non-BCD hex → top byte) | **Authoritative**: PS5 SDK at procparam +0x14 of `eboot.bin` (ELF directly; SELF → unwrap in memory; encrypted → unknown). Fallback: `param.json` `sdkVersion` only (not required FW), accepted when 1..30. Show both. |
| Required FW | `param.json` `requiredSystemSoftwareVersion` → `FW 12.7` style | Same, shown as "Required FW" |
| Suggested target | `clamp(detected − 1, 1, 10)`, raised to ≥ 4 when detected ≥ 5; overwrites the dropdown unless the user locked it | No auto-change. The default target is the setting (4); entries above the game SDK are disabled. |
| Eboot SDK (Batch Retag) | Byte-scan for `ORBI`; the u64 at magic−8 must equal `0x60`; then the u32 at magic+12 is mapped through the pair table, or `(raw>>24)` if 1..10 and `(raw & 0x00FF0000) == 0` | Replaced by procparam parsing |

### 2.17 AMPR index (`ampr_emu.index`, AMPRIDX3), GPL-3.0 port of drakmor upstream

| Part | Layout |
|---|---|
| Header (48 bytes, `<8sIIQQQII`) | magic `AMPRIDX3`; u32 version = 3; u32 record_size = 24; u64 record_count; u64 path_blob_size; u64 hash_offset; u32 slot_size = 16; u32 slot_count |
| Records (24 bytes each, `<IIQq`), sorted by key | u32 path_offset (into the blob); u32 path_len (without NUL); u64 size; i64 mtime (Unix seconds) |
| Path blob | UTF-8 `/app0/<rel>` + NUL, in record order |
| Padding | zeros up to `hash_offset = align16(48 + 24·count + blob)` |
| Hash slots (16 bytes, `<QII`) | u64 hash; u32 index+1 (0 = empty); u32 flags (bit 0 = another record has the same 64-bit hash) |
| Slot count | smallest power of two ≥ 2·count (minimum 2) |
| Hash | FNV-1a 64 (basis `0xcbf29ce484222325`, prime `0x100000001b3`) over the key bytes; 0 → 1 |
| Probe | `pos = hash & mask`; linear `pos = (pos+1) & mask`; records are inserted in sorted order |
| Key (**upstream, required**) | `\` → `/`, UTF-8 encode, then fold **only** bytes `A`–`Z` to lower case. Sort and dedupe by these bytes. |
| Key (**ours, a bug**) | Python `str.lower()` on Unicode, hashing code points. Non-ASCII paths get a different hash and order than the runtime expects. |
| mtime over FTP | Upstream: `MDTM`/`MLSD` `YYYYMMDDHHMMSS` UTC → Unix seconds. **Ours (bug)**: the integer `YYYYMMDDHHMMSS`. |
| Exclusions | Local: the output file and `.tmp`; upstream also `/app0/ampr_commands.bin` and `/app0/apr_emu.log` (FTP: also `.bak`) |
| Rejected | Paths with `\t`, `\n` or `\r` (skipped with a warning); case-insensitive collisions → error (exit 3) unless allowed (keep the first) |
| Write | `.tmp` then replace. FTP upload: upstream moves the old index to `.bak`, renames the new one in, and restores it on failure. Ours deletes the old index first. |
| Triggers | (a) every exFAT/FFPKG/PFS build whose source has `fakelib/libSceAmpr.sprx` writes `ampr_emu.index` **into the source dump** (`ui/tab_unibuild.py:2329`, collisions allowed, failure non-fatal); (b) the Advanced-tab tool for a local folder or `ftp://` with upload (`ui/tab_advanced.py:633`) |

### 2.18 Related tools in the Backports screen (not in Backport GUI)

| Tool | Behaviour | Hazards → C++ |
|---|---|---|
| SDK Fixer (`E:15736`) | Re-patches the SDK marker only (fakelib off, libc patch off) on a folder (full dump or backport files). Copy mode → `<name> SDK <N>` (name cleaned of FW tags, IDs and `- Copy`); overwrite mode keeps `.bak`. Verifies: no fakelib added, libc pattern not introduced, `param.json`/`param.sfo` unchanged, output exists. Restores executables on failure. | On an **ELF-only** folder, the engine patches the **source** in place even in copy mode. C++ (`SdkFixer.*`, job `backport.sdkFix`): staging only. Copy → `<output>/<name> SDK <N>` via `.fwpart`, executables and param files checked by SHA-256, the rest by size, removed on failure or cancel; the title ID stays in the name. In place → a verified backup ZIP `<prefix> SDK <N> fix original files.zip` in the originals ZIP folder, then the apply journal (rollback, start-up recovery); no `.bak`. The finished journal is kept in `BackportBackup/<TitleID>/…/sdk-fixes/` and undoes the fix (`backport.restore {game, undoSdkFix: true}`). A folder with its own backport record gets an `sdk_fixes` entry, so restore and the S7 verdict recognise the fixed files. |
| Batch SDK Retag (`E:16514`) | For each `*.zip` in a source folder: free temp space ≥ 1.05 × uncompressed size; `extractall` → find `eboot.bin` (root or one level down) → read its SDK → skip if equal / unreadable / no eboot; warn if it crosses SDK 6; engine (no fakelib, no libc, no backup) on the eboot's folder; copy back only the executables that existed; ZIP the whole tree to `<out>/<retagged name>.part` → rename. The output folder must differ from the source. | Becomes Backport GUI's "Rebuild ZIP with another SDK" (single ZIP and batch) |
| Results (`E:13665-13950`) | Settings list `bpr_log` of `{game, orig_zip, pat_zip, status pending/worked/failed, ts}`. **It worked** deletes the **originals ZIP** immediately, with no confirmation. **Didn't work** deletes the backport ZIP. Also: add manually, clear completed, clear all (keeps files). | D10: no automatic deletion. Never delete the originals ZIP while the manifest says the game is patched. |
| Notes field | Saved as `abp_notes`; not written anywhere else | Stored in the manifest and the results entry |
| Audio fix scanner / test patch (`ui/audio_*.py`) | Read-only scanners (cluster `c7 45 94 00 0C`, `c7 03 0C 00 00 00`, `6a 0C 58`; profile DBSparklingZero PPSA15210) and a gated test-patch on a copy. Its sub-tab `advfixes` is **not registered** (unreachable). | C++: report only (`AudioScan.*`, job `backport.audioScan`): the cluster (12 or 8 channels), twin branches, audio API names, loose leads and the two confirmed profiles (DBSparklingZero PPSA15210, SpiderMan2 PPSA03016), on the raw bytes of `eboot.bin` (or every executable). No test patch: profiles need console evidence. |

### 2.19 Python defects the C++ must not copy

| # | Defect | Evidence |
|---|---|---|
| P1 | ELF-only dumps are SDK-patched in place, leaving `.bak` in the game | A.3, `EngineInPlaceRegression` |
| P2 | Encrypted SELFs are "decrypted" into garbage | A.4 |
| P3 | A dump's own fakelib files are re-signed and SDK-patched | A.4 |
| P4 | A failing file still yields a patched game and ZIPs | §2.2 step 12 |
| P5 | Failures double-counted (downgrade + signing) | A.4 |
| P6 | Restore leaves `fakelib/`, so the next run is refused | §2.15 |
| P7 | "It worked" deletes the originals ZIP without asking | §2.18 |
| P8 | The SDK dropdown offers 11 (clamped to 10 with a warning); the "file" fakelib mode is offered but always refused | `panel_bp_auto.py:501`, `E:16847` |
| P9 | Game SDK falls back to required FW | §2.16 |
| P10 | Unwrap assumes `e_phoff == e_ehsize`; truncated reads are silent | §2.7 |
| P11 | AMPR key folding and FTP mtime | §2.17 |
| P12 | Backport All has no free-space check; the temp-path check uses `patched/<game>` while the longest real path is `patched/decrypted/<rel>` | §2.2, `backport_safety.temp_path_problem` |
| P13 | The Games scan covers only 2 levels and does not exclude the ZIP folders | `tab_backports.py:1607` |
| P14 | SDK Fixer copy mode modifies an ELF-only source (fixed in C++: the source is only read) | §2.18 |

---

## 3. Safety rules the C++ must keep

S1–S30 are rules already in force (from `core/backport_safety.py`, `STATUS.md` and the flows). S31–S38 are new rules for C++.

| ID | Rule | Origin | Applies to |
|---|---|---|---|
| S1 | No fallback patching: if the engine can't run, nothing changes and the run fails clearly. (The old "byte-patch mode" wrote the SDK into `e_flags` / SELF header bytes.) | CL, IA M1 | all |
| S2 | No engine code runs from a user-writable or stale location. (Python: a fresh private `exfat_builder_bp_*` folder each run, removed from `sys.path`.) C++: native code, no extracted scripts. | ST M7, IA M2 | all |
| S3 | Target SDK must be a supported pair (1–10). C++ refuses or disables; it never clamps silently. | E:16847, E:22525 | all |
| S4 | Target SDK ≤ the game's own SDK → otherwise "Target SDK N is higher than this game's own SDK M" | ST | single, batch |
| S5 | `eboot.bin` must be at the game root | E:16823 | single, batch |
| S6 | Fakelib state `same` → refuse "Already backported": originals can't be captured from a patched folder | ST | single, batch |
| S7 | Manifest verdict `patched` → refuse; `differs` → confirm (single) or skip (batch) | ST | single, batch |
| S8 | Fakelib source: must exist and be a folder; must not be inside the game or contain it (`is_within` via normalised `commonpath`); `.sprx`/`.prx` must sit **directly** in the folder (no sub-folders; `__MACOSX` dirs, `._*`, `.ds_store`, `thumbs.db`, `desktop.ini` ignored); each ≥ **1024 bytes**; each must have ELF/SELF magic; at least one library | backport_safety | single, batch |
| S9 | The originals ZIP folder is **required** and is chosen and checked **before** any file changes | ST | single, batch |
| S10 | ZIP destination: set; exists; not inside the game; `len(abs(dest/name)) + len(" (99).part") ≤ 259`; writable (probe file `.exfat_builder_write_test.tmp` created and removed). The backport folder is checked with the worst-case name `FW 10xx <prefix> backport files.zip`. | backport_safety | both folders |
| S11 | Temp path ≤ 259 characters for the longest executable under staging (C++: compute the exact longest path, including `unwrapped/`) | backport_safety | single, batch |
| S12 | Free space before starting (Python: temp ≥ 4 × executable bytes, single run only). C++: staging (3 × executables + fakelib) + ZIP estimate (1.1 × executables + param + `decrypted/`) on each destination volume, plus a margin, **for every game in a batch** | E:16933 | all |
| S13 | Snapshot every executable before anything changes; any copy error stops the run | ST | single, batch |
| S14 | Another tool's fakelib files that get replaced are kept in the originals ZIP | ST | single, batch |
| S15 | OS metadata files and dirs are never executables (AppleDouble `._*`, `.DS_Store`, `Thumbs.db`, `desktop.ini`, `ehthumbs.db`, `.localized`, `__MACOSX`, `.Trashes`, `.fseventsd`, `.Spotlight-V100`, `.TemporaryItems`, `System Volume Information`, `$RECYCLE.BIN`) | CL, test_backport_engine | scan |
| S16 | `.bak` files are never executables; only engine-created `.bak` are removed, pre-existing ones are kept (C++: none created) | test_backport_engine | scan |
| S17 | ZIPs: `.part` → CRC read-back → member count → rename; never overwrite (`(N)` suffix); a ZIP error is a failure | backport_safety | all ZIPs |
| S18 | The temp originals are deleted only after the originals ZIP is written and read back; otherwise the log says where they are | ST | single, batch |
| S19 | Any decrypt, patch, sign or ZIP failure → not reported as success | ST | all |
| S20 | Restore: zip-slip entries refused and reported; a partial restore is a failure | backport_safety | restore |
| S21 | A manifest is recorded after each run (originals hashes + post hashes), written atomically | ST | single, batch |
| S22 | Backport All runs the same per-game checks without dialogs; doubtful → skip, nothing changed | ST | batch |
| S23 | Every archive handle is closed (Windows rename/delete) | ST, AtomicZip test | all |
| S24 | SDK Fixer: fakelib off, NID patch off, `param.*` unchanged, verified; copy mode removes a half-written copy; overwrite mode restores the executables on failure | E:15736 | SDK Fixer |
| S25 | Batch Retag: source ZIPs never modified; output folder ≠ source; temp free ≥ 1.05 × uncompressed; `.part` + rename | E:16514 | retag |
| S26 | No issue is closed or commented on by tooling | ST | process |
| S27 | Licence: no Backport GUI code; no `right.sprx`, PSSR, kernel files, firmware import tables, or the Backport GUI's AMPR/PlayGo source copied, bundled or downloaded | ST | build |
| S28 | Paths over 259 characters: C++ may use `\\?\` internally (`fw::toLongPath`), but ZIP output names stay ≤ 259 (Explorer and other tools) | backport_safety | all |
| S29 | The "Partial" state must be explicit: the game is patched but a ZIP is missing → the log names the originals' location | E:17880 | all |
| S30 | A ZIP folder inside the game is refused (it would be packed into the image and re-scanned) | backport_safety | all |
| S31 (new) | Nothing in the game folder changes before the Apply phase (D1) | §2.2 | all |
| S32 (new) | Encrypted or compressed SELFs are detected while scanning; default: refuse before anything changes (D2) | §2.7 | all |
| S33 (new) | `fakelib/` and `decrypted/` are never processed (D3) | §2.10 | all |
| S34 (new) | Any per-file failure → no backport ZIP, no apply (D4) | [R] | all |
| S35 (new) | Apply uses a journal with rollback and offers recovery at start-up (§2.12) | [T] | apply |
| S36 (new) | Restore removes added files and verifies hashes (D6) | §2.15 | restore |
| S37 (new) | No automatic deletion of the originals ZIP (D10) | §2.18 | results |
| S38 (new) | The backport and originals folders are excluded from game scans [T]; nested folders (one strictly inside the other) are refused; equal folders are allowed because the names differ | [T] | scan, preflight |

---

## 4. Parity matrix

Legend: **Y** = in our Python, **P** = partial, **N** = no. C++ plan: **Impl** = v1; **Later** = after v1; **Blocked** = needs the D:\ source, licence-clear data, or documented values.

| # | Backport GUI feature | Src | Py | C++ plan | Reason / note |
|---|---|---|---|---|---|
| 1 | Library scan of added folders, depth 6, `eboot.bin` at root | R,I | P (2 levels) | Impl | `backport.scanDepth` |
| 2 | Columns: cover, title + path, PPSA, SDK, Required FW, status | S5 | P (cards) | Impl | |
| 3 | Drag and drop of folders | R | N (Backports) | Impl | |
| 4 | Search, select all/none, multi-select, scan locations, rescan, clear list | S5 | P | Impl | |
| 5 | Title/PPSA/version/SDK/FW from `param.json` or folder name | R | Y | Impl | shared GameMetadata |
| 6 | Ignore invalid param SDK values; SDK from `eboot.bin` | C | P | Impl | D7 |
| 7 | Backport and originals folders never scanned | T | N | Impl | S38 |
| 8 | Batch run with per-game status and counters (Games / Selected / Ready / Running / Done / Failed-partial) | R,S5 | P (sequential, log only) | Impl | JobQueue rows |
| 9 | Retry failed | R | N | Impl | |
| 10 | Cancellation | R | N | Impl | Cancel before Apply leaves the game untouched; during Apply → rollback |
| 11 | SDK rewrite + fake-sign | R | Y (pairs 1–10) | Impl | §2.5–2.8 |
| 12 | Bundled default fakelib provider set ("nothing to configure") | R,I | N (user folder, default None) | Blocked | Provider binaries of unknown provenance (incl. AMPR `82484486`/`0aadd31d`, PlayGo `a0f20f40`) |
| 13 | Import-table audit FW 4.03–13.60 | R | N | Blocked | Firmware import tables of undocumented origin; D:\ source |
| 14 | Confirmed fix families (automatic fixes) | R | N | Blocked | D:\ source; per-family evidence |
| 15 | Audio check across firmware bands | R,T | N | Blocked | Needs the firmware tables (#13) |
| 16 | ZIP CRC re-read + recorded hashes | R | P (CRC + count; hashes recorded, not re-checked) | Impl | Re-hash every member against the manifest |
| 17 | Signed-executable structure check | R | N | Impl | Re-parse each SELF, unwrap, compare with the patched ELF |
| 18 | Fakelib set check | R | P (pre-run only) | Impl | Also after the build: staged fakelib = source hashes |
| 19 | Refuse to output a failing build | R | N | Impl | D4 |
| 20 | Backport ZIP `FW 4xx <PPSA> <Title> (<ver>) backport files.zip` | R | Y | Impl | §2.13 |
| 21 | Originals ZIP with only the replaced files | R | P (superset) | Impl (superset) | Superset kept for restore and re-patch |
| 22 | Never overwrite | R | Y | Impl | `(N)` suffix |
| 23 | Per-run manifests with SHA-256 of every file | R | P | Impl | Schema 2 |
| 24 | `_backport_details/` (loose files, reports, manifests) | I | N (`BackportBackup/<ID>/manifest.json`) | Impl (run report) | `RunReport.*`: every run that passes preflight (and every SDK fix and audio report) writes `<stamp> <kind>.json` + a self-contained `.html` (inline CSS, no scripts, CSP `default-src 'none'`, every value escaped) to `BackportBackup/<TitleID>[/folders/<name>-<hash>]/reports/`; `manifest.json` and its place are unchanged. Inputs, target pair, per-file results (rewritten / unchanged / added / replaced / left unchanged / failed, with why), skipped files, ZIP paths and SHA-256s, verification, apply journal, issues, warnings. A failed run names its report in the error detail. "Open report" on the Backport page. No loose-file copy. |
| 25 | Apply to game folders (opt-in), after a verified backup | R | Always on, no option | Impl | D5 |
| 26 | Write journal, automatic rollback, rollback offer at next start | R,T | N | Impl | §2.12 |
| 27 | Restore originals (Settings) | R | Y (single, multi) | Impl | D6 |
| 28 | Target SDK "SDK N · loads on FW N.00+" | S5 | P (4–11, 11 clamped) | Impl | 1–10, default 4 |
| 29 | Target SDK 11–13 | R | N | Blocked | No documented pair values |
| 30 | Target ≤ game SDK | R | Y | Impl | S4 |
| 31 | Rebuild a ZIP with another SDK (row menu) | R | P (Batch SDK Retag) | Impl | Single and batch; fakelib/NID off. The "keeps FW 4.xx save fixes" part is blocked (#39). |
| 32 | PS5 Pro fix | R | N | Blocked | Fix set and evidence only in the D:\ source |
| 33 | PS5 Pro build / PSSR 2 test (+ from an existing backport) | C,S5 | N | Blocked | PSSR files (game-derived), AGC library change, D:\ source |
| 34 | Switch AMPR / PlayGo | N,S5 | N | Impl (user-supplied) / Blocked (bundled) | `ProviderSet.*`, setting `backport.amprPlaygoFolder`: the user's own `libSceAmpr` / `libScePlayGo` (S8 rules) take the place of the same-named libraries wherever the fakelib set is staged (also replacing a dump's own copies, kept in the originals ZIP). SHA-256 and size pinned per folder in `<dataDir>/backport/provider-pins.json` at the first backport (trust on first use); a changed, added or missing file is refused (`ProviderChanged`) until re-pinned (`backport.pinProviders`, "Pin them again" on the page). Bundling ampr_emu 0.4.2.1 (GPL-3.0, upstream LICENSE verified) needs reproducible-build provenance first. Engine providers: blocked. Swapping the providers inside an existing backport ZIP: not built (a new backport with the folder set does it). |
| 35 | `right.sprx` "Invalid param magic" → standard copy | C | N (counted as failure) | Blocked (replacement) / Impl (detection) | The standard copy is Sony-derived. C++: S32 + policy `leaveUnchanged` (console result **UNVERIFIED**) |
| 36 | Dump with another tool's fakelib (#27) | R | P (overlay + backup; extras re-signed) | Impl | Overlay, extras unchanged with a warning (D3). Replacing extras → #34. |
| 37 | OS metadata ignored | R | Y | Impl | S15 |
| 38 | Already-backported detection | T | Y (fakelib + manifest) | Impl | S6, S7 |
| 39 | Save-data fixes (FW 4.xx), save-mount check | IA, S5 | N | Blocked | D:\ source |
| 40 | Audio fixes (0.3.1 families, CRI 7.1.4 → 7.1) | IA | P (hidden scanner, one profile) | Impl (report only) / Blocked (patches) | `backport.audioScan`: the legacy scanner's patterns as a read-only report (§2.18). Patch profiles need console evidence; the audio band check stays blocked (#15). |
| 41 | Unity / Il2Cpp module-name case (#2) | IA | N | Impl (detector) / Blocked (fix) | `ModuleImports.*`: DT_NEEDED and DT_SCE_NEEDED_MODULE names from the dynamic table in DYNLIBDATA (string table at DT_SCE_STRTAB from the segment start; fallback: the NUL-separated names in DYNLIBDATA), compared with the files in `sce_module/` case-sensitively. A name that matches only when case is ignored is a Warn issue (`ModuleNameCase`) in the plan and the library scan. Nothing is renamed. That PS5 executables use these tag values is **UNVERIFIED** on real titles. |
| 42 | Destination rules (both set, different, not nested, not in game, free space, folder named in the message) | T | P (backport folder optional; same folder allowed; nesting not checked) | Impl | S9, S10, S12, S38 |
| 43 | Free-space check | R | P (single only) | Impl | S12 |
| 44 | Failure message set (higher SDK / SDK N refused / no safe backport / no backport needed / Partial) | T | P | Impl | §6.3 |
| 45 | Console Logs: payload send to ELF loader (9021), live klog | R | P (klog viewer on 3232, payload sender) | Impl (shortcut) | The Backport page links to the PS5 connect page's Payloads and Kernel log tabs (no new protocol work; the features are the PS5 connection module's). |
| 46 | Crash dump auto-download ZIP, hang snapshots | R | N | Later | PS5 connection module. The console paths of crash dumps are not documented in this repository, so no download was built. |
| 47 | Export logs ZIP (activity, per-run plan/log/report/trace, settings, system; no game files) | RI | P (text report) | Impl | `<Desktop>/…_logs_<date>_<time>.zip` |
| 48 | Report a problem with prefilled form | C,BR | P (link) | Impl | GitHub issue-form query parameters: title, PPSA, version, SDK, fakelib |
| 49 | Settings: subfolder depth, open logs folder, restore, applied list | I | P | Impl | §5 |
| 50 | Activity "Next run: …" plan line; tip box; tooltips | S5 | P | Impl | Plan line from `BackportPlan::summary()` |
| 51 | Engine identity in the status bar ("backend <hash>") | S5 | N | Impl | Engine version + SDK/NID table hash |
| 52 | Engine unpacked to `%LOCALAPPDATA%\D97B` | I | Analogue: `extract_backport` | N/A | Native |
| 53 | Linux / macOS builds | R | N | Out of scope | |

Our features not in Backport GUI:

| Feature | Python | C++ plan |
|---|---|---|
| SDK Fixer (SDK marker only, copy or overwrite) | Y | Impl (`backport.sdkFix`; staging only, P14 fixed; in place = backup ZIP + journal, undoable) |
| Batch SDK Retag | Y | Merged into #31 |
| libc NID patch (SDK ≤ 6 apply / > 6 revert) | Y | Impl (data-driven, `backport.nidPatchMode`) |
| Results tracker | Y | Impl without automatic deletion (D10) |
| Restore multiple by TITLE_ID | Y | Impl |
| AMPR index (build-time + tool + FTP upload) | Y | Impl (GPL port of current upstream; fixes P11) |
| Apply backport folder to a mounted image or ffpkg | Y (File Manager, ffpkg editor) | Owned by those modules |
| Profile manager / target switcher, audio fix sub-tab | Code present, sub-tabs unregistered | Folded into the manifest and restore; audio: read-only report (#40) |
| Notes per game | Y (stored only) | In the manifest and results |

---

## 5. Settings (section "Backport", schema in `fw::SettingsSchema`)

| Key | Type | Default | Validation | Effect | Legacy key |
|---|---|---|---|---|---|
| `backport.originalsZipFolder` | DirPath | "" | S10; required to start; not nested with `backportZipFolder` unless equal | Originals ZIP and `BackportBackup/` location | `abp_zip_orig_dest` |
| `backport.backportZipFolder` | DirPath | "" | S10 when set. Required when `applyToGameFolder` is off (otherwise the run produces nothing). | Backport ZIP location; empty = no backport ZIP | `abp_zip_pat_dest` |
| `backport.targetSdk` | Enum `1`…`10` | `4` | Must be a pair in the table; at run time ≤ the game SDK (S4) | SDK pair to write; labels "SDK N · loads on FW N.00+" (perJob) | `abp_target_sdk`, `abp_last_sdk` (values > 10 dropped with a warning) |
| `backport.fakelibFolder` | DirPath | "" | S8 (a file path is dropped with a warning) | Fakelib set to stage into `fakelib/` and nested eboot dirs (perJob) | `abp_fakelib` |
| `backport.amprPlaygoFolder` | DirPath | "" | S8 rules for the provider names; pins (#34) | The user's own `libSceAmpr` / `libScePlayGo`, staged in place of the same-named libraries (perJob) | — |
| `backport.applyToGameFolder` | Bool | `true` (**owner decision**; Backport GUI: false) | — | Apply phase on or off (perJob); the plan line shows "game folder patched" or "game folder unchanged" | — |
| `backport.nidPatchMode` | Enum `auto` / `off` | `auto` | — | `auto` = Python behaviour (≤ 6 apply, > 6 revert) (advanced) | — |
| `backport.encryptedSelfPolicy` | Enum `refuse` / `leaveUnchanged` | `refuse` | — | S32. `leaveUnchanged` copies such files untouched and marks the run Partial (experimental, **UNVERIFIED** on console) (advanced, perJob) | — |
| `backport.verifyBuild` | Bool | `true` | — | Post-build checks: SELF re-parse and round trip, ZIP re-hash, fakelib hashes (advanced; off only for diagnosis) | — |
| `backport.scanFolders` | StringList | [] | Existing dirs; de-duplicated | Library sources | `bp_folders` + `bp_scan_folder` |
| `backport.scanDepth` | Int 1–12 | `6` | range | Sub-folder depth for game discovery | — |
| `backport.keepStagingOnFailure` | Bool | `true` | — | Keep the staging folder for diagnosis when a run fails (advanced) | — |
| `backport.journalRetentionDays` | Int 1–365 | `30` | range | Apply journal retention (advanced) | — |
| `backport.notes` | String ≤ 500 | "" | length | Written into the manifest and results entry (perJob) | `abp_notes` |
| `backport.results` (history store, not a setting) | — | — | — | Results list | `bpr_log` (import) |
| `playgo.amprIndexOnBuild` (owner: Build/PlayGo) | Bool | `true` | — | Write `ampr_emu.index` when `fakelib/libSceAmpr.sprx` exists | — |
| `playgo.amprAllowCaseCollisions` | Bool | `true` for builds, `false` for the tool | — | Keep the first entry on a collision | — |
| Temp folder (owner: core) | DirPath | system temp | writable | Staging location | `temp_dir` |
| `abp_output` | — | — | — | Unused in Python; not imported | `abp_output` |

---

## 6. C++ module design: `native/src/engines/backport/`

Namespace `fw::backport`. Errors use `fw::Result<T>` / `fw::Error` (`core/Result.h`). Cancellation uses `CancellationToken::checkpoint()` in every per-file and per-chunk loop. Progress uses `ProgressReporter` phases. File I/O goes through `core/FileUtil` (`PartFile`, `TempDir`, `sha256File`, `copyFileStreaming`, `isSameOrInside`, `toLongPath`, `diskSpace`). The ZIP writer and reader come from a core `ZipWriter`/`ZipReader` (zlib, ZIP64, CRC re-read). If core does not provide them, `engines/backport/ZipIo.*` is a temporary home.

### 6.1 Files and classes

| File | Class / API | Responsibility | Licence note |
|---|---|---|---|
| `SdkTable.h/.cpp` | `struct SdkPair { int n; quint32 ps5; quint32 ps4; }`; `const SdkPair* sdkPair(int n)`; `int bcdMajor(quint32)`; `QString firmwareLabel(int n)`; `QByteArray tableHash()` | §2.6 | facts |
| `ElfImage.h/.cpp` | `struct ElfHeader`, `struct ProgramHeader`; `class ElfImage { static Result<ElfImage> parse(QByteArrayView, Strictness); … }` | §2.4 validation; bounds-checked accessors | original |
| `SceParam.h/.cpp` | `struct ParamInfo { int phdrIndex; ParamKind kind; quint64 offset; quint32 size; quint32 magic; quint32 ps4; quint32 ps5; }`; `Result<std::vector<ParamInfo>> findParams(const ElfImage&)`; `Result<PatchReport> patchSdk(QByteArray& elf, const SdkPair&)` | §2.5 (clean-room) | facts only |
| `SelfFormat.h` | constants, `SelfHeader`, `SelfEntry`, props bit helpers | §2.7–2.8 | GPL-3.0 (port of make_fself; attribution header) |
| `SelfReader.h/.cpp` | `enum class SelfKind { FakeSigned, Encrypted, Compressed, Malformed }`; `Result<SelfInfo> inspect(QByteArrayView)`; `Result<QByteArray> unwrap(QByteArrayView)` | §2.7 (C++ column) | GPL-3.0 (derivative) |
| `SelfWriter.h/.cpp` | `struct SignParams { quint64 paid = 0x3100000000000002; quint64 ptype = 1; quint64 appVersion = 0, fwVersion = 0; }`; `Result<QByteArray> makeFakeSelf(QByteArrayView elf, const SignParams&)` | §2.8; byte parity with Appendix A | GPL-3.0 (make_fself, john-tornblom; Nazky changes) |
| `NidPatch.h/.cpp` | `struct NidSubstitution { QByteArray from, to; int boundarySdk = 6; }`; `NidResult applyNid(QByteArray& self, int targetSdk, NidMode)` | §2.9 | facts only |
| `ExecutableScanner.h/.cpp` | `enum class FileClass { Elf, Self, SelfEncrypted, SelfCompressed, NotExecutable, Excluded }`; `struct ExecFile { QString rel; qint64 size; FileClass cls; QString sha256; std::optional<int> sdk; }`; `Result<ScanResult> scanGame(const QString& root, const CancellationToken&)` | §2.3 + S15/S16/S33 | original |
| `FakelibSet.h/.cpp` | `Result<QStringList> validateSource(dir, game)`; `enum class FakelibState { None, Same, Other }`; `FakelibState stateFor(game, src)`; `QStringList extrasInGame(game, src)` | §2.10, S8 | original |
| `GameSdk.h/.cpp` | `GameSdkInfo { optional<int> ebootSdk, paramSdk; optional<QString> requiredFw; QString source; }` | §2.16 / D7 | original |
| `BackportOptions.h` | `BackportOptions { QString game; int targetSdk; QString fakelibDir, originalsDir, backportDir; bool applyToGame; NidMode nid; EncryptedPolicy enc; bool verify; QString notes; bool confirmDiffers; }` | built from `SettingsValues` + overrides | — |
| `BackportPlanner.h/.cpp` | `struct Issue { IssueCode code; Severity sev /*Refuse, Confirm, Warn*/; QString message; QStringList files; }`; `struct BackportPlan { ScanResult scan; QList<Issue> issues; QString originalsZipPath, backportZipPath; qint64 needBytes; QString summary() const; }`; `Result<BackportPlan> plan(const BackportOptions&, const CancellationToken&)` | S3–S12, S30–S33, S38 → plan or refusal; nothing written except the S10 probe | original |
| `BackportEngine.h/.cpp` | `Result<BuildResult> build(const BackportPlan&, const QString& stagingDir, JobContext&)` → per-file `FileResult { rel; Stage stage; Status st; QString message; optional<int> sdkBefore, sdkAfter; QString shaBefore, shaAfter; }` | Snapshot → unwrap → patch → sign → NID → stage fakelib → verify (§6.4) | original orchestration |
| `BackupArchive.h/.cpp` | `QString zipPrefix(const GameMetadata&, QString fallback)`; `QString originalsZipName(prefix)`, `backportZipName(prefix, sdk)`; `Result<ZipWritten> writeOriginals(…)`, `writeBackport(…)` (unique path, `.part`, CRC + hash re-read) | §2.13, S17 | original |
| `BackportManifest.h/.cpp` | `struct Manifest` (schema 1 + 2 fields); `Result<Manifest> read(path)`; `Status write(path, Manifest)`; `enum class Verdict { Patched, Differs, Original, Unknown }`; `std::pair<Verdict, QStringList> verdict(game, Manifest)` | §2.14 | original |
| `ApplyJournal.h/.cpp` | `class ApplyJournal { static Result<ApplyJournal> begin(…); Status applyAll(JobContext&); Status rollback(); static QList<PendingJournal> findInterrupted(); }` | §2.12, S35 | original |
| `RestoreService.h/.cpp` | `Result<RestoreReport> restore(game, zip, optional<Manifest>, token)`; `optional<QString> safeMemberTarget(root, name)`; `QString findOriginalsZip(game, dir)` | §2.15, S20, S36 | original |
| `AmprIndex.h/.cpp` | `Result<AmprStats> buildLocal(root, out, allowCollisions, token)`; `Result<AmprStats> buildFromListing(entries, out, …)` (FTP listing supplied by the PS5 module) | §2.17 | GPL-3.0 (drakmor ampr_emu `tools/build_ampr_index.py`, current upstream) |
| `RetagZip.h/.cpp` | `Result<RetagResult> retag(zip, targetSdk, outDir, JobContext&)` | §2.18 / matrix #31 | original |
| `BackportJob.h/.cpp` | `class BackportJob : public fw::Job` kind `backport.game`; `RetagJob` (`backport.retagZip`); `RestoreJob` (`backport.restore`); `AmprIndexJob` (`backport.amprIndex`) | Queue integration; phases: Checking, Copying originals, Unwrapping, Patching SDK, Signing, Verifying, Writing ZIPs, Applying, Recording | — |
| `BackportSettings.cpp` | `void registerBackportSettings(SettingsSchema&)` incl. `addLegacyKey` | §5 | — |
| `ResultsStore.h/.cpp` | Results list in the history store; `bpr_log` import | §2.18, D10 | — |

### 6.2 Data flow per file (BackportEngine)

| Class from scanner | Stage 1 | Stage 2 | Stage 3 | Stage 4 | Output |
|---|---|---|---|---|---|
| `Elf` | copy → `originals/` | `patchSdk` on the in-memory copy | `makeFakeSelf` | `applyNid` | `stage/<rel>` |
| `Self` (fake-signed) | copy → `originals/` | `unwrap` → `unwrapped/<rel>` (for diagnosis) → `patchSdk` | `makeFakeSelf` | `applyNid` | `stage/<rel>` |
| `SelfEncrypted` / `SelfCompressed` | — | — | — | — | policy: refuse the run (planner) or carry the file unchanged (not staged; Partial) |
| under `fakelib/`, `decrypted/` | — | — | — | — | not processed; extras listed |

Files are processed one at a time in memory (executables are well under 2 GiB; a file larger than `kMaxExecBytes = 1 GiB` → `Unsupported`, **UNVERIFIED** limit). Work is parallelised across files with a bounded pool, and the results are joined in path order.

### 6.3 Outcomes and error cases

| Outcome / code | When | User message (sentence case, names the file or folder) | Game changed? |
|---|---|---|---|
| `Succeeded` | All files OK, ZIPs written and verified, Apply (if on) verified | "Backported to SDK N (loads on FW N.00+). Originals: …zip" | only if Apply was on |
| `Partial` | Build OK but the backport ZIP or Apply failed and was rolled back; or `leaveUnchanged` files present | "Built, but …" plus the originals location | no (rolled back) / yes when only `leaveUnchanged` |
| `Failed` | Any per-file failure (D4) or a ZIP failure before Apply | "No safe backport was produced: <file>: <reason>" | no |
| `NotAGame` | no `eboot.bin` | "eboot.bin is not in <folder>. Pick the game's root folder." | no |
| `TargetAboveGameSdk` | S4 | "Target SDK N is higher than this game's own SDK M. Choose SDK M or lower." | no |
| `AlreadyBackported` | S6/S7 | "This folder already contains backport files…" + files | no |
| `DiffersFromOriginals` | S7 differs | confirm (single) / skip (batch) | no |
| `FakelibInvalid` | S8 | list of problems | no |
| `ZipFolderInvalid` | S9/S10/S30/S38 | reason + folder | no |
| `PathTooLong` | S11/S10 | path and length | no |
| `InsufficientSpace` | S12 | needed / free per volume | no |
| `NothingToDo` | no executables, or every file already at the target with the same bytes | "No backport is needed" | no |
| `ElfInvalid` | §2.4 rule broken (sub-reason) | "<file> is not a PS5 executable this tool can rewrite (<rule>)" | no |
| `ParamMissing` / `ParamSizeInvalid` / `ParamMagicInvalid` | §2.5 | Python wording + "the file may still be encrypted" for magic | no |
| `SelfEncrypted` / `SelfCompressed` / `SelfMalformed` | §2.7 | "<file> is still encrypted…" | no |
| `SignUnsupported` | §2.8 | "The SDK N build was refused: <file> …" | no |
| `ZipWriteFailed` / `ZipVerifyFailed` | S17 | reason; originals location | no (Apply not reached) |
| `ApplyFailed` | journal step | "Applying failed at <file>; the game folder was rolled back" | rolled back |
| `Cancelled` | token | "Cancelled. The game folder was not changed." / "…rolled back." | no |

Map these to `fw::ErrorCode`: NotFound (NotAGame), AlreadyExists (AlreadyBackported), InvalidArgument (target, folders), InsufficientSpace, CorruptData (ELF/SELF/param), Unsupported (encrypted, sign), IoError, VerificationFailed, Cancelled.

### 6.4 Build verification (before any ZIP or Apply)

| Check | Pass condition |
|---|---|
| V1 SELF structure | `SelfReader::inspect` succeeds; kind FakeSigned; `num_entries == 2·\|Q\|`; offsets ascending and 16-aligned; `file_size` matches |
| V2 Round trip | `unwrap(stage/<rel>)` (with the NID substitution reversed for comparison) == the patched ELF bytes |
| V3 SDK | `findParams(unwrap)` shows the target pair in every param segment |
| V4 Fakelib | Every staged fakelib file's SHA-256 == the source file's |
| V5 Coverage | Every scanned executable outside `fakelib/` and `decrypted/` is staged (or `leaveUnchanged` by policy) |
| V6 ZIP | After writing: CRC test + every member's SHA-256 == the staged or original file hash |
| V7 Apply | Every applied file's SHA-256 == the staged hash |

### 6.5 Test plan (Qt Test, `native/tests/backport/`)

Fixtures are generated in code by `SyntheticElf.h` from the tables below, so no game data or Sony binaries are needed. Golden values come from the Python engine (Appendix A).

| Fixture | Exact definition |
|---|---|
| **F1** (executable, 0x5A20 bytes) | Header: `e_ident 7F 45 4C 46 02 01 01` + 9 zeros; `e_type 0xFE10`; `e_machine 0x3E`; `e_version 1`; `e_entry 0x1000`; `e_phoff 0x40`; `e_shoff 0`; `e_flags 0`; `e_ehsize 0x40`; `e_phentsize 0x38`; `e_phnum 5`; `e_shentsize 0x40`; `e_shnum 0`; `e_shstrndx 0`. Phdrs `(type, flags, offset, vaddr, paddr, filesz, memsz, align)`: `(1,5,0x1000,0,0,0x4800,0x4800,0x4000)`, `(1,6,0x5800,0x8000,0x8000,0x100,0x100,0x4000)`, `(0x61000001,4,0x5800,0x8000,0x8000,0x60,0x60,8)`, `(0x61000000,4,0x5900,0,0,0x100,0x100,0x10)`, `(0x6FFFFF01,4,0x5A00,0,0,0x20,0x20,1)`. Bytes `0x1000..0x57FF` = `(offset*7) & 0xFF`. At `0x5800`: u64 `0x60`, u32 `0x4942524F`, u32 1, u32 `0x09590001`, u32 `0x07000038`. `0x5900..0x59FF` = `0xAB`. `0x5A00..0x5A1F` = ASCII `VERSIONDATA-0123456789abcdefXYZ!`. Every other byte is 0. |
| **F2** (PRX, 0x3200 bytes) | `e_type 0xFE18`, `e_entry 0`, `e_phnum 4`, other header fields as F1. Phdrs: `(1,5,0x1000,0,0,0x1000,0x1000,0x4000)`, `(1,6,0x2000,0x4000,0x4000,0x80,0x80,0x4000)`, `(0x61000002,4,0x2000,0x4000,0x4000,0x40,0x40,8)`, `(0x61000000,4,0x3000,0,0,0x200,0x200,0x10)`. Bytes `0x1000..0x1FFF` = `(offset*13+1) & 0xFF`. At `0x2000`: u64 `0x40`, u32 `0x3C13F4BF`, u32 1, u32 `0x10590001`, u32 `0x08000041`. At `0x3000`: `printf#A#B\0 4h6F1LLbTiw#A#B\0 malloc#A#B\0` (no spaces). Every other byte is 0. |
| F3 | F2 with the module param size = 0 (u64 0 at `0x2000`) → patch "no param, skipping" = success, unchanged |
| F4 | F1 with the magic at `0x5808` = `0xDEADBEEF` → `ParamMagicInvalid 0xDEADBEEF` |
| F5 | F1 with the param size = `0x10` → `ParamSizeInvalid` |
| F6 | F1 with the PROCPARAM phdr changed to type `0x7` → `ParamMissing` |
| F7 | `SELF(F2)` with bit 1 set in every data entry's props and the data XORed with `0x5A` → `SelfEncrypted` (Python: `Invalid param magic`, A.4) |
| F8 | `SELF(F2)` with bit 3 set → `SelfCompressed` |
| F9 | `SELF(F1)` with magic bytes `54 14 F5 EE` → unwraps the same as F1 |
| F10 | F1 with `e_phoff 0x80` (header padded) → C++ unwrap round trip OK (Python would misplace the phdrs) |
| F11 | Truncated: `F1[:0x5000]` → `CorruptData` (sign); `SELF(F1)[:0x1000]` → `SelfMalformed` |
| F12 | F1 with `e_machine 0x28` / `EI_CLASS 1` / `EI_DATA 2` / `e_phentsize 0x40` / `e_type 3` → `SignUnsupported` / `ElfInvalid` (one test each) |
| F13 | F1 with two PROCPARAM phdrs at different offsets → both patched |
| G1 | Synthetic game trees: (a) SELF-only (eboot + `sce_module/libc.prx` from F2); (b) ELF-only; (c) mixed; (d) with `fakelib/` from another tool plus an extra `libSceAmpr.sprx`; (e) with an encrypted `sce_sys/about/right.sprx` (F7); (f) OS metadata (`._eboot.bin`, `.DS_Store`, `__MACOSX/`); (g) pre-existing `eboot.bin.bak`; (h) nested `sub/eboot.bin` |

| Test group | Cases |
|---|---|
| SdkTable | All 10 values; BCD decode (`0x10000040` → 10); labels; table hash stable |
| ElfImage | Parse F1/F2; F11/F12 errors; bounds |
| SceParam | F1 pair 4 → procparam bytes `60000000000000004f524249010000000100040931000004`; pair 10 → `…0100091240000010`; F3–F6, F13 |
| SelfWriter | **Golden SHA-256** (Appendix A) for F1/F2 × pairs 4/10; header fields of F1 (§2.8 worked example); `e_shnum` = 0 in the copy; version tail appended |
| SelfReader | `unwrap(golden SELF)` == patched ELF; F7–F11 |
| NidPatch | `SELF(F2 pair 4)` → golden `6f930788…`; idempotent (`already_patched`); revert at SDK 7; `off` mode; a file without the pattern is unchanged |
| ExecutableScanner | G1 (f), (g); `fakelib/` and `decrypted/` excluded; unreadable fallback by name |
| FakelibSet | Port `FakelibSource` and `FakelibState` from `tests/test_backport_*.py` (nested, undersized, non-ELF, empty, inside the game, none/same/other) |
| Planner | Port `test_preflight_ok_and_refusals`, `test_preflight_fakelib`; S12 space (fake `diskSpace`); S11 lengths; S38 nested folders |
| BackupArchive | Port `AtomicZip` (unique name, failure leaves nothing, handles closed); case-insensitive dedupe; prefix rules (`_sfo_zip_prefix` cases: `PPSA15552 Dead Cells (01.000.000)`, `PPSA03598-app`, `[DLPSGAME.COM]-Phantom Breaker 01.000 PPSA13794`) |
| Manifest | Read a Python-written schema-1 JSON fixture; port `ManifestDetection` (patched, original, old manifest without post_hashes → differs, `tree_hashes` wrapper strip); schema-2 round trip |
| Restore | Port the zip-slip cases (`../escape.txt`, `/abs.txt`, `C:/win.txt`, `sub\..\..\evil.txt`, `D:x`, `\\srv\s\x`); partial restore = failure; added files removed; manifest hash mismatch refused |
| Engine integration | G1 (a)–(h): the game folder's bytes are unchanged after build-only; staged outputs are SELF; G1(b) has no `.bak` anywhere; G1(d) extras untouched + warning; G1(e) refused by default / Partial with `leaveUnchanged`; one failing file → Failed, no backport ZIP, game unchanged |
| ApplyJournal | Inject a failure after k files → rollback restores the bytes exactly; simulate a crash (journal left `pending`) → `findInterrupted` → rollback; cancel during Apply |
| AmprIndex | Small tree including `Ä.txt`, `b.TXT`, `a/B.bin`: header fields, ordering by ASCII-folded bytes, FNV values, duplicate-flag, collision error vs allow; golden bytes cross-checked once against upstream `tools/build_ampr_index.py` @ `cfa85df` (dev step, not in CI) |
| RetagZip | ZIP of G1(a) built at pair 7 → retag to 4: eboot SDK 4, name `FW 4xx …`, source untouched, `_retagged_` collision rule |
| Cross-implementation (dev-only script, not shipped) | Run the decoded Python engine on F1/F2 and compare its hashes with the C++ output (this document's Appendix A was produced this way) |

---

## 7. Blocked on the D:\ source or on licence-unclear data

| Item | Blocked by | Unblock path |
|---|---|---|
| Firmware import-table audit (FW 4.03–13.60), fix families, audio band gate | Tables of undocumented provenance; Backport GUI code has no licence | Tables from a documented source (e.g. the owner's own firmware dumps with written provenance), then a clean-room design |
| PS5 Pro fix | Fix set only in the D:\ source | Owner documents the changes and evidence |
| PS5 Pro build / PSSR 2 test | PSSR files are game-derived; AGC library change | Not bundleable; at most a user-supplied files option after review |
| `right.sprx` standard copy | Sony-derived binary | None for bundling. The C++ offers `leaveUnchanged` (console result **UNVERIFIED**). |
| Default fakelib provider set (AMPR `82484486`/`0aadd31d`, PlayGo `a0f20f40`, others) | Unknown provenance | User-supplied folder (current behaviour) |
| ampr_emu 0.4.2.1 binaries | GPL-3.0 is fine, but build provenance and toolchain (SDK headers) are unverified | Reproducible build from upstream + licence text + source offer |
| SDK pairs 11–13 | No documented values | Values from a documented source |
| FW 4.xx save fixes, save-mount check, Il2Cpp case fix details, seven uncommitted changes on `snapshot/20261021-f1-pro-v45` | Only in the D:\ source | Read-only inspection brief `docs/farewell/BACKPORT_HANDOFF.md` on the owner's PC |

---

## Appendix A: experiments run for this spec (Linux, Python 3, decoded embedded engine)

| ID | Experiment | Result |
|---|---|---|
| A.1 | Golden fixtures F1/F2 (§6.5) through patcher → make_fself → decrypt_fself | F1 ELF `00af10a0e02eb93b2ec8d989a4103a32dd4cbb9b90e392a099a580b6aa65c08a`; F1 pair 4 ELF `065462fac5402013fa5c15cb9777f5e6ff1eb7e1dc24c77d6e38aca2bda395f7`, SELF (0x5080 bytes) `10b80a5af227262378e246bcc723e2d92b34f1b48d6c839aaef5e43d109151f8`; F1 pair 10 ELF `0e839ea9a64af2d1bf5db04b760219c97acb498b50d3f0b9af06521e4909e04c`, SELF `00ddda8c3e02a4357a50a50430feb3bd82f77e6a2ffd2acb97bef3123bd876fb`. F2 ELF `215eacb6dd2d12b7a281905466c6d3c902ba33806e2361dd95d72bd9ae4ef771`; F2 pair 4 ELF `ea50e46a3831d8ae613aa67b7096b99002eb10c5e9f95d785e3c51db831177c8`, SELF (0x1880 bytes) `5d49441f2285515e94e80ece8f30383f1792ad722e33717933f88a0c70d236db`; F2 pair 10 ELF `b8cdf74f84056f601a4dccbd66c2dba6f86d2c3011b7e7b6fe97769c4f423ae1`, SELF `ba8002953a29b054610fc7663b21285d5e6dd01220897326355f15df04332c72`. Unwrap round trip identical in all four cases. |
| A.2 | NID patch on the F2 pair 4 SELF | 1 occurrence replaced → `6f93078810d17f6862d9ca29aa1c7f75a73232f11098213cb94e7f00f6a1bc68` |
| A.3 | `decrypt_and_sign_pipeline` on an ELF-only game (eboot + PRX, pair 7) | The game's `eboot.bin` was SDK-patched **in place** (still ELF), with `eboot.bin.bak` and `libfoo.prx.bak` left in the game; output holds the SELFs |
| A.4 | Pipeline on a SELF game with `fakelib/` (module with param size 0), libc pattern, and an "encrypted" `sce_sys/about/right.sprx` (bit 1 set, data XOR) | All 4 SELFs "decrypted" (no encryption check). `right.sprx` → `Invalid param magic 0x6649AEE5`, counted in downgrade **and** signing. The fakelib module was re-signed ("no param, skipping" = success). libc: 2 occurrences replaced. `.bak` files under `out/decrypted/`. Game unchanged (SELF case). |
| A.5 | Earlier fixture with PROCPARAM outside any PT_LOAD | Round trip lost the param bytes: PROCPARAM is not a SELF segment (§2.4). Fixtures must put params inside a PT_LOAD. |
| A.6 | Upstream comparison | Embedded Backport.py, make_fself.py, decrypt_fself.py and patcher are byte-identical to `Nazky/Auto-Backpork@dab6fad` (no LICENSE file). `ui/build_ampr_index.py` is an older revision of `drakmor/ampr_emu@cfa85df tools/build_ampr_index.py` (GPL-3.0) and lacks the upstream ASCII-fold, Unix-mtime and FTP `.bak` fixes. |
