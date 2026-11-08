# Migration matrix — legacy Python app → native C++ Farewell Edition

Source inventories: `LEGACY_INVENTORY.md` (engines, 108 settings keys, 173
rows), `LEGACY_UI_INVENTORY.md` (pages, PS5 connectivity, library, i18n),
`FPKG_PARITY.md`, `BACKPORT_PARITY.md`. Module docs: `docs/engines/*.md`,
`docs/UI.md`.

**Status values**

| Status | Meaning |
|---|---|
| **Done** | Implemented natively; automated tests pass on Linux (GCC, ASan+UBSan) and Windows (MSVC 2022, Qt 6.8) CI. |
| **Done · field check** | Implemented and tested against fakes/stand-ins/goldens, but the real counterpart (PS5 console, Sony Publishing Tools, a real game dump on the owner's PC) has not been exercised. |
| **Superseded** | The legacy mechanism is replaced by a better native one (reason given). |
| **Blocked** | Cannot be built honestly yet (licence, missing source or data) — reason given. |
| **Not ported** | Deliberately dropped (reason given). |
| **Ported · build pending** | Code and tests written from the owner's D:\ Backport source (2026-11-04, branch `claude/backport-parity-v5`), not yet compiled or run: the PC that did the port has no C++ tool chain yet and could not push to CI. Becomes Done after the Windows build, CTest and CI pass. |

Test names refer to `native/tests/**/tst_*.cpp`.

## 1. Image building

| Legacy feature | C++ implementation | Status | Tests | Remaining |
|---|---|---|---|---|
| exFAT image build (OSFMount + format.com + robocopy, admin) | `engines/exfat` native writer from the Microsoft spec: bare volume, 512 B sectors, label OSFIMG, legacy cluster rule (64 KiB if avg file ≥ 1 MiB else 32 KiB), legacy size formula, contiguous NoFatChain files, sparse zero runs; no admin, no OSFMount | Done · field check | tst_exfat_roundtrip/format/jobs/bench; fsck.exfat clean; FUSE exfat read-back (manual) | Mount on a real PS5 and in Windows |
| Image size override, cluster/sector settings, hidden-file exclusion | `exfat.*` settings (18), legacy keys mapped | Done | tst_exfat_jobs (every setting read) | — |
| FFPKG (UFS2) build via UFS2Tool (.NET 8) | `engines/ufs2` native port of UFS2Tool `newfs -D` with the legacy parameters; byte-identical to UFS2Tool 4.0.0 (9 layouts); UTF-8 names (UFS2Tool wrote `?`) | Done · field check | tst_ufs2_reference (UFS2Tool images rebuilt byte for byte), compat (fsck_ffs, dumpfs, UFS2Tool fsck/ls/extract) | Console mount test |
| FFPFSC / FFPFS (mkpfs / Lazy_MkPFS) | `engines/pfs` GPL port of Lazy_MkPFS: folder→.ffpfs, folder→.ffpfsc (two-step, nested image in memory), image→.ffpfsc, AMPR index, multithreaded deterministic compression | Done · field check | tst_pfs_golden (20 cases byte-identical to Lazy), roundtrip, jobs, benchmark | zlib-ng/ISA-L back-ends not ported (zlib only) |
| Build-tab metadata, naming presets, collision handling | `engines/gameinfo` (metadata chain, Naming), `PartFile`/`uniquePath`, UI Build page | Done | tst_gameinfo, tst_core | — |
| Output-inside-source refusal, FAT32 refusal (≥ 4 GiB), cloud/network warnings | `Job::validate`, `checkOutputVolume()` | Done | tst_core_extra, engine job tests | — |
| Free-space pre-flight (+ reserve) | `ensureFreeSpace` + `storage.freeSpaceMarginMiB` | Done | tst_core, engine tests | — |
| Previous image set aside / restored on failure (audit M2) | `PartFile::commit(replace)` with unique `.fw-old` | Done | tst_core, tst_exfat_jobs, tst_ufs2_jobs | — |
| Delete source after verified build (audit C1/C2) | `general::deleteSourceAfterVerifiedBuild` + per-engine rules (full verification only) | Done | tst_settings_jobs, tst_pfs_jobs, tst_ufs2_jobs, tst_exfat_jobs | — |
| Auto-retry failed builds | `general.autoRetryCount/DelaySeconds` in the queue (transient errors only) | Done | tst_settings_jobs | — |
| Embedded .bat/.ps1, OSFMount discovery/sweep/dismount chain, robocopy parsing | — | Superseded | — | No mounting needed: images are written and read natively |
| Post-build FTP upload (`adv_auto_upload`) | `integration::PostBuildActions` (`ps5.autoUploadAfterBuild`, verified images only) | Done | tst_integration | — |
| Shutdown/restart/sleep after the queue | `QueueCompletionWatcher` + `power::perform` + countdown sheet | Done (Windows action itself: field check) | tst_settings_jobs | — |

## 2. Verification, extraction, editing, conversion

| Legacy feature | C++ implementation | Status | Tests | Remaining |
|---|---|---|---|---|
| Tri-state verdict, verify before "Complete!" (audit M1) | `JobOutcome::verification`; Failed ⇒ job failed | Done | tst_settings_jobs | — |
| Strict per-file comparison (audit H1) | exFAT/UFS2/PFS verify (full = every byte, structure = names/sizes/structure) | Done | engine tests | — |
| Verify an image without its source | exFAT standalone verify (structure + read-back), UFS2/PFS likewise | Done | tst_integration | — |
| mkpfs verify `--source-dir` rules (audit H2) | PFS verify against the build's own file list | Done | tst_pfs_jobs | — |
| Extract exFAT (OSFMount + robocopy) | native reader → `<out>.part` → verify → rename | Done | tst_exfat_roundtrip/jobs | — |
| Extract ffpkg (Dokan mount / UFS2Tool) | native reader, Windows name rules, collisions renamed | Done | tst_ufs2_jobs | — |
| PFS unpack (+ deep) | `pfs::extractImage` | Done | tst_pfs_roundtrip | — |
| ffpkg "mount as drive" (Dokan) | — | Not ported | — | Needs a kernel driver; extraction covers the use |
| Edit exFAT (add/replace/delete/new folder) | `image.exfat.edit`: in place with undo journal when safe, else rebuild + verify + swap | Done | tst_exfat_edit; tst_app_ui, tst_image_tools (Image tools › Edit against a real built image, staged changes applied) | — |
| Edit ffpkg (add/replace/delete/rename/mkdir) | `image.ffpkg.edit` rebuild through `.part`, original kept until verified | Done | tst_ufs2_jobs; Image tools › Edit | Not exercised from the UI end to end |
| Convert exFAT↔ffpkg↔.ffpfs/.ffpfsc | `image.convert`: repack (extract + build + verify), or direct for .exfat/.ffpkg → .ffpfsc (the image file packed as is under a plain ASCII inner name, no temporary copy; such a container converts back by unpacking it); `replaceExisting` sets an existing output aside until the new image verified | Done | tst_integration (round trip byte-identical; direct with a Unicode name and back to exFAT byte-identical, to FFPKG verified against the dump, .ffpfs target, replace); tst_image_tools (Image tools › Convert submits direct and .ffpfs jobs, replace confirmation) | Field check: ShadowMount+/MicroMount mounting an .ffpfsc that holds an .exfat/.ffpkg |
| ffpkg hex preview, compare two images | `image.compare` {a, b, depth}: `integration::compareTrees` over `integration::ImageFiles` (native readers for .exfat / .ffpkg / .ffpfs / .ffpfsc incl. nested containers, or a folder); only in A / only in B (files and folders), different size, different content with the first differing offset (full depth streams both sides), PFSC block padding tolerated; capped lists + exact counts in `JobOutcome::details`, job succeeds whatever it finds. Image tools › Verify against a folder or image + result sheet (Copy report). Hex preview: `ImageFiles::readSlice` → `services/HexPreview` (worker thread), Image tools › Edit › Preview… / double-click, offset/hex/ASCII, 64 KiB + Load more | Done | tst_image_compare (identical exFAT/FFPKG/FFPFSC/dump pairwise, edited copy → every difference kind, structure vs full, PFSC padding with a real compressed .ffpfs, capped lists, cancellation, validation, byte slices from every format); tst_image_tools (hex service on built .exfat/.ffpkg, Preview sheet with Load more, Compare card → queue → result sheet) | Legacy report's UFS2 mode/BSD-flag differences not compared (the readers do not expose them for every format); the preview opens what the editor opens (.exfat/.ffpkg) |

## 3. FPKG packages (see FPKG_PARITY.md)

| Feature | C++ implementation | Status | Tests | Remaining |
|---|---|---|---|---|
| Build FPKG from a dump folder with the user's Publishing Tools | `engines/fpkg` (GP5, param.json patches, PlayGo layouts A/B/C, scanner rules, junction aliases, tool runner, header check, conflict policies) | Done · field check | tst_fpkg_rules (GP5/scenario/param goldens from Python), tst_fpkg_build (end-to-end with a stand-in tool) | Run with real Sony tools; console install |
| Build from .exfat/.ffpkg/.ffpfsc/.gp5 | `fpkg::SourceStager` + integration stagers; GP5 reader | Done | tst_integration, tst_fpkg_build | — |
| Update packages (base package), add-ons (`prospero_ac`) | `fpkg.build {basePackage}`, `fpkg.addon` | Done · field check | tst_fpkg_build | Real tools |
| Verify / extract existing .pkg | `fpkg.verify`, `fpkg.extract` via the user's tool | Done · field check | tst_fpkg_build | Real tools |
| Reference app settings (PSVIETHOA v3.0.0) | 24 `fpkg.*` settings + PlayGo/Verification; built-in engine (Kraken/PFS v3) and online metadata deliberately absent | Done | tst_fpkg_rules | — |
| Live CPU-core limit, Defender pre-scan, keep-awake | `setLiveCpuLimit`, `fpkg.prescan`, app-wide keep-awake | Done | tst_fpkg_build; FPKG page live CPU combo | — |
| Bundled Sony tools / LibProsperoPkg built-in engine | — | Not ported | — | Licence: Sony binaries are never bundled; built-in engine out of scope |

## 4. Backport (see BACKPORT_PARITY.md, docs/engines/BACKPORT.md)

| Feature | C++ implementation | Status | Tests | Remaining |
|---|---|---|---|---|
| SELF unwrap → SDK patch (pairs 1–10) → fake-sign, libc import rename | `engines/backport` (clean-room patcher, GPL port of make_fself) | Done · field check | tst_backport_format (all Appendix A golden hashes) | Console load test |
| Scan, planner with safety rules S1–S38 | `ExecutableScanner`, `BackportPlanner` | Done | tst_backport_scan/planner/run | — |
| Backport + originals ZIPs, read-back verification | `BackupArchive` on `core/Zip` | Done | tst_backport_zip, tst_zip | — |
| Apply to game folder (journaled, crash-safe rollback), restore | `ApplyJournal`, `RestoreService` | Done | tst_backport_run | Default "apply" = on (owner may flip) |
| Already-backported detection (manifest hashes) | `BackportManifest` (reads Python schema 1, writes 2) | Done | tst_backport_run | — |
| Interrupted-apply prompt at start | `backport::interruptedApplies()` | Done | tst_backport_run; start-up banner on the Backport page | — |
| Run report per run (JSON + self-contained HTML next to `BackportBackup/<ID>/`), "Open report" on the Backport page | `RunReport` (runner, SDK fix, audio report) | Done | tst_backport_report | — |
| SDK Fixer, SDK marker only (copy, or in place with backup ZIP + journal, undo) | `SdkFixer`, `backport.sdkFix`, `backport.restore {undoSdkFix}` | Done · field check | tst_backport_sdkfix | Console load test of a fixed build |
| Unity/Il2Cpp module-name case (detector) | `ModuleImports` → plan / library-scan issue `ModuleNameCase` (DT_NEEDED against `sce_module/`, report only) | Done · field check | tst_backport_detect | The D:\ review found the owner's engine reads PS5 tags 0x61000043/45/47/49, not the PS4 values this detector uses: likely silent on real titles. Superseded for the fix by the row below |
| Unity/Il2Cpp module-name case **fix** (issue #2, MGS4) | `ModuleCaseFix`: spellings from `"%s/Modules/<Name>.prx"` in the decoded eboot; staged, zipped and applied under the game's spelling (case-only rename, journaled `rename_to`); originals ZIP, rollback and Restore keep the dump's (`renamed_files` in the manifest) | Ported · build pending | tst_backport_modulecase | Console confirmation pending in the owner's records |
| Audio-fix scanner (legacy hidden tab) | `AudioScan`, `backport.audioScan` (read-only report) | Done | tst_backport_detect | Patch profiles need console evidence (no test patch) |
| CRI Unity audio fix (12-channel rack → 8, issues #10/#11) | `CriAudio`: "CRI Atom" module, exactly one 12-channel rack in an executable segment with the 8-channel rack 1..0x40 bytes before it, 3 bytes; setting `backport.criAudioFix` (default on, as the owner's engine) | Ported · build pending | tst_backport_criaudio (owner's vectors) | Hardware: PAC-MAN WORLD 2 Re-PAC, STORY OF SEASONS, WONDERBOY (base FW 4.50, owner) |
| PS5 Pro fix, shader-assert part | `ProFix`: guarded `s_trap 1` → `s_nop 0` in the root eboot.bin, setting `backport.proFix` (off), ZIP `… Pro fix backport files.zip` | Ported · build pending | tst_backport_profix | The Quality60 AGCDriver hook (second Pro fix part) is a third-party Sony library build: not bundled |
| Standard `right.sprx` substitute (issues #2/#7/#8) | `RightSprx`: an unconvertible game-root `sce_sys/about/right.sprx` takes the user's own standard copy (`backport.standardRightSprx`, SHA-256 `81544cbb…` pinned), converted normally; the dump's copy stays in the originals ZIP | Ported · build pending | tst_backport_rightsprx (synthetic file through a test seam) | Sony's file is never bundled; no console result recorded for a substituted build |
| Target SDK 11–13 | `SdkTable` pairs from retail process params (owner's `sdk_tool.EXTRA_PAIRS`, 44/33/3 titles) | Ported · build pending | tst_backport_format, planner, scan, sdkfix | Untested on hardware (only SDK 5 has a reported result) |
| Canonical AGC/AGCDriver/PSML trio | `CanonicalTrio`: pins (name, size, SHA-256) as data; a game holding one pinned file is refused as already backported; a fakelib folder that is the trio is named | Ported · build pending | tst_backport_detect | The libraries are Sony's (from another tool's backport): never bundled |
| Plain-ELF fakelib dump note | Planner Info note with the `PRX_PROCESS_STARTUP_FAILURE 0xa0020104` lead, never a refusal | Ported · build pending | tst_backport_detect | — |
| Apply/Restore safety from the Backport GUI export layer | Every check before the first write (nothing written on a mismatch); read-only game files made writable; Apply/rollback/Restore copies flushed to the drive before the rename; "Change the game folders?" confirmation on the Backport page; Target SDK, Pro fix and CRI fix shown on the Backport page (Target SDK appeared on the FPKG page before) | Ported · build pending | tst_backport_run (gameChangedBeforeApplyWritesNothing, fileAppearingBeforeApplyWritesNothing, readOnlyGameFilesAreAppliedAndRestored), tst_backport_scan | Per-user applied ledger, Retry failed/Cancel/counters on the page, Report-a-problem prefill: not built (gap report) |
| User-supplied AMPR/PlayGo libraries with hash pinning | `ProviderSet`, `backport.amprPlaygoFolder`, `backport.pinProviders` | Done · field check | tst_backport_providers | Nothing bundled; whether a given provider loads is the user's test |
| Console logs from the Backport page (payload, kernel log) | Links to the PS5 connect page tabs | Done | — (UI link; screenshot `backport-report`) | — |
| Crash-dump auto-download and hang snapshots (Log Sender) | — | Not ported (spec ready) | — | Fully specified from the D:\ Log Sender (`/user/devlog/system/sce_coredumps.0` over ftpsrv 2121, klog `[CRS][coredump_seq]` wake-up, ps5debug-NG on 744 with the PC listening on 755); needs a PS5 to test, so it was left for a later pass |
| Import-table audit FW 4.03–13.60, audio firmware gate (Set3DLatency bands) | — | Blocked | — | The tables (39 `fwdb/<fw>.json`, 37 usable) were built from the owner's own unpacked firmware with no written provenance; the gate needs an x86 decoder (Zydis/Capstone) for mode and result checks. Needs the owner's provenance note before any table ships |
| Code fix families that need x86 decoding (save-data Prepare 6→0, systemBlocks sentinel and the double request #22-#24, absolute-mouse fallbacks, Wwise 7.1.4→7.1, GamePresets 0x30→0x28, Set3DLatency non-fatal, libc heap semantic path) and import-gap families (lazy GOT seeds, DirNameSearch2) | — | Blocked | — | Capstone in the owner's engine (needs a decoder in C++) or the firmware tables above |
| PS5 Pro build / PSSR 2 test, AGCDriver hook, default fakelib set, bundled AMPR/PlayGo providers, libc reuse table | — | Blocked | — | Sony/game-derived binaries (overlay PSML `72855f03`, `k9.psp`, Quality60 AGCDriver `810c3b20`, canonical trio, other games' libc); PSSR 2 also needs an ELF symbol/relocation writer and the audio gate |

## 5. Queue, history, logging, settings, storage

| Legacy feature | C++ implementation | Status | Tests | Remaining |
|---|---|---|---|---|
| Unified queue (add/remove/move/clear, states, cancel, pause, retry, per-item overrides) | `jobs/JobQueue` + Queue page + Jobs dock | Done | tst_settings_jobs, tst_app_ui | — |
| Queue save/load, restart recovery | `queue.json`; running jobs restored as Interrupted (never auto-restarted) | Done | tst_settings_jobs | — |
| History page (stats, filter, CSV, clear) | `JobHistory` (JSON lines) + `exportCsv` + Queue/Home | Done | tst_settings_jobs | — |
| Session log, export, crash handler, bug report with redaction | `core/Log` (text + JSON lines, rotation), Logs page, `crash::install` (minidump), `tools.report` | Done | tst_core, tst_tools, tst_app_ui | — |
| Settings file (108 keys) | Schema-driven `SettingsStore`; every setting declared by its consumer; legacy import (`addLegacyKey` + whole-file importers) | Done | tst_settings_jobs, per-module "every setting is read" tests | — |
| Settings page, Advanced page, reset | Generated Settings page (sections, groups, validation, overrides, reset) | Done | tst_app_ui | — |
| Temp sweep, clear temp, temp size | `Paths::sweepStaleTemp`, `tempUsageBytes` | Done | tst_core_extra | — |
| (new) One job per image/game at a time; deletes never follow junctions; one app instance | `Job::busyPaths` + queue scheduling; `removeTreeNoFollow`; `QLockFile` in the data folder | Done | tst_settings_jobs, tst_core_extra | — |
| Child processes killed on close (audit H3/H4) | Windows job objects (suspended start), POSIX process groups | Done | tst_core (spawned grandchild killed) | — |
| Update check + self-update | `connect/update` (native installer only, SHA-256 required, fails closed) | Done · field check | tst_update (local HTTP server) | First real release |
| Translations (16 languages) | — | Not ported | — | Legacy tables covered 62 of 1,559 strings; Qt Linguist infrastructure to follow |
| Discord webhook (hidden `discord_webhook` key, POST on finish) | `connect/notify` (`DiscordNotifier`, `discordPayload`): real setting `general.discordWebhookUrl` (General, advanced; empty = off; only discord.com / discordapp.com webhook addresses), legacy key imported, one embed per finished run from `QueueCompletionWatcher` (counts + up to 10 jobs; folders cut to file names; mentions disabled; update checks and diagnostics left out), nothing sent with `FW_OFFLINE`; token never logged and hidden in problem reports | Done · field check | tst_notify (local HTTP stub: POST body and headers, 404 reported, offline/empty/invalid send nothing, legacy import), tst_tools (report redaction), tst_legacy_import, tst_ps5_tools_ui (setting) | A real Discord channel was not contacted |
| Write benchmark, theme | — | Not ported | — | Dead/orphan keys in the legacy app |

## 6. Library, dump rename, tools

| Legacy feature | C++ implementation | Status | Tests | Remaining |
|---|---|---|---|---|
| Library scan, metadata, covers | `engines/gameinfo` (`library.scan`), Library page | Done | tst_gameinfo, tst_app_ui | — |
| Dump Rename (presets, preview, no overwrite) | `library.rename` (preview/apply/undo log) | Done | tst_gameinfo; tst_app_ui (rename preview through the engine) | — |
| Language Stripper | `engines/tools` (verified backup ZIP before removal, restore); legacy scan crashed on an undefined table — fixed | Done | tst_tools | — |
| DLC-emu editor | `tools::dlcemu` (parse/render/validate/auto-find/save with .bak) | Done | tst_dlcemu (goldens from the legacy module); tst_app_ui (editor round trip) | — |
| AMPR index (generate locally) | `engines/pfs/AmprIndex` (build-time, Lazy rules); stand-alone tool `ampr.index.local` (`AmprJobs`, current ampr_emu upstream row rules, "Not an APR title?" gate) on PS5 connect › AMPR index | Done | tst_pfs_golden (AMPR case); tst_ampr_ftp (local tool, gate); tst_app_ps5_tools (AMPR tab queues it) | Windows CI not yet run on this change |
| AMPR index over FTP | `ampr.index.ftp` (integration `AmprFtp`): app0 check before any walk, `walkRemoteTree` (MLSD / MDTM times), native writer, upload `.tmp` → old index kept as `.bak` (upstream fix) → rename, read-back; PS5 connect › AMPR index with a console folder picker | Done · field check | tst_ampr_ftp (index from the fake console == index from the same local folder byte for byte, MLSD and LIST+MDTM; `.bak`; parent folder refused; rename refused; cancel); tst_ps5_content (walk) | The AMPR emulator reading an index built over FTP on a real console |
| Fakelib manager (ShadowMount+) | `ps5::fakelib` (list, legacy reject/confirm rules, add/replace via `uploadReplace`, remove, enable/disable by rename without overwriting the other folder); PS5 connect › Fakelib | Done · field check | tst_ps5_content (fakelibLocalFileRules, fakelibManagerOverFtp, fakelibKeepsTheOriginalWhenTheServerCannotRename); tst_app_ps5_tools (fakelibTabManagesOverlaysOnAFakeConsole) | ShadowMount+ loading the overlays on a console |
| PS5 Manager game list and compare | `ps5::listConsoleGames` (LIST/MLSD per folder, SITE AVAIL), `mergeGames` (legacy statuses), `compareConsoleFolder` (missing / extra / different size against a dump or an .exfat/.ffpkg); PS5 connect › Manager | Done · field check | tst_ps5_content (gameListMergesConsoleAndLocalImages, folderCompareFindsMissingExtraAndDifferentFiles, siteAvailAndFolderParsing, remoteTreeWalkWithMlsdAndList); tst_app_ps5_tools (managerTabListsGamesAndComparesFolders) | Real console listings and SITE AVAIL replies (inventory U4); large games over Wi-Fi |

## 7. PS5 connectivity (see docs/engines/PS5_CONNECT.md)

| Legacy feature | C++ implementation | Status | Tests | Remaining |
|---|---|---|---|---|
| FTP upload/download (serial, no resume) | `FtpClient` + `FtpTransfer`: parallel connections, REST resume, retries, protected-path guard, partial-file cleanup | Done · field check | tst_ps5_ftp (in-process FTP server); tst_app_ui (FTP browser against the fake server) | Real console FTP server |
| Payload sender | `PayloadSender` (`ps5.payload.send`) | Done · field check | tst_ps5_services | Real console |
| Kernel log viewer | `KlogClient` (reconnect, tags, wait-for-line) | Done · field check | tst_ps5_services; tst_app_ui (kernel-log model bounds) | Real console |
| Y2JB (DPI install, auto-block with backups and read-back) | `DpiInstaller`, `Y2jb` | Done · field check | tst_ps5_services | Real console |
| ShadowMount+/MicroMount config + Safe to unplug | `MountConfig`, `ps5.shadowmount.stop` | Done · field check | tst_ps5_services (INI goldens from the legacy code) | Real console |
| IP auto-detect sweep | `ps5::discoverConsoles` (/24 of every IPv4 interface, ports 2121/2122, 0.3 s, 50 probes on a private `QThreadPool`, cancellable, greeting captured); PS5 connect › Find my PS5 sheet fills `ps5.consoleIp`/`ps5.ftpPort` | Done · field check | tst_ps5_diagnostics (subnetsSkipLoopbackAndLinkLocal, discoveryFindsListenersOnInjectedHosts, discoveryIsCancellable), tst_ps5_tools_ui (findMyPs5FillsTheAddress, cancellingTheSearchChangesNothing) | A real network with a console |
| FTP diagnostics and 16 MiB speed test (FTP-18) | `ps5::diagnoseFtp` + job `ps5.ftp.diagnose` (reachable/RTT, banner, login, FEAT, PASV/EPSV, TYPE I, writable probe, destination, zftpd score; speed test STOR/RETR with byte-for-byte read-back and cleanup, leftovers reported); PS5 connect › Diagnostics tab; Home setup checklist (MGR-5) | Done · field check | tst_ps5_diagnostics (diagnosticsPassAgainstTheFakeServer, passiveFallbackAndRefusal, unreachableAndRefusedLogin, notWritableAndLeftoverProbeAreReported, speedTestMeasuresBothWaysAndCleansUp, speedTestReportsCorruptionAndLeftover, cancelledSpeedTestRemovesItsFile, diagnoseJobEndToEnd), tst_ps5_tools_ui (diagnosticsTabRunsTheJob, diagnosticsShowAnUnreachableConsole, homeChecklistCountsTheSteps) | Real console FTP servers; the ZHTTP web-UI probe is not ported |
| Autoloader editor (local `ps5_autoloader/autoload.txt`) | `ps5::autoloader` (legacy normalisation and header, validation, insert/remove helpers, save with `.bak`, add/delete payloads); setting `ps5.autoloaderFolder` (legacy `al_dir`); PS5 connect › Autoloader | Done | tst_ps5_content (autoloaderNormalizesLikeTheLegacyEditor, autoloaderParsesAndValidates, autoloaderEditingHelpers, autoloaderLoadSaveAddDelete, autoloaderFolderSettingAndLegacyKey); tst_legacy_import; tst_app_ps5_tools (autoloaderTabEditsALocalFolder) | Windows CI not yet run on this change; ps5_autoloader itself never run against the saved file |

## 8. Packaging and platform

| Item | Implementation | Status | Evidence |
|---|---|---|---|
| Windows executable | MSVC 2022 + Qt 6.8.3, windeployqt, manifest (PerMonitorV2 DPI, long paths, asInvoker) | Done | `native-windows.yml`: build, tests, deployed-exe self-test, page screenshots, 30 s launch check |
| exFAT images with the real Windows driver | `tst_exfat_windows`: image wrapped in a VHD, mounted with Mount-DiskImage, read back and `chkdsk`ed; a Windows-formatted exFAT volume read by the native reader | Done | Windows CI (test log printed in the run) |
| Installer + portable zip + SHA256SUMS | Inno Setup (per-user, no admin), zip, checksums | Done | CI artifacts |
| No admin rights, no .NET, no OSFMount, no Python | Native engines | Done | — |
| Windows CI acceptance of the release candidate | Commit `b29d7b5`, run 41804192710 | Done | 36/36 tests, deployed-exe self-test, 41 page screenshots, 30 s launch, installer + portable zip + SHA256SUMS |
| Real PC acceptance test (owner's machine) | `native/packaging/pc-acceptance.ps1`: build, CTest, deploy, `--self-test`, `--screenshot-pages`, 30 s real launch, portable zip, SHA-256, `SUMMARY.md` in `_PC_TEST_EVIDENCE\<version>-<commit>\` (private app data folder; nothing installed) | Pending | 2026-11-04: blocked on the PC itself — no MSVC, Windows SDK, CMake, Ninja or vswhere; the Visual Studio Build Tools install needs a UAC prompt and the Microsoft licence terms (owner only). The script wrote `BLOCKER.md` with the exact install command |
