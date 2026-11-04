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
| Edit exFAT (add/replace/delete/new folder) | `image.exfat.edit`: in place with undo journal when safe, else rebuild + verify + swap | Done | tst_exfat_edit; tst_app_ui (Edit image page against a real built image) | — |
| Edit ffpkg (add/replace/delete/rename/mkdir) | `image.ffpkg.edit` rebuild through `.part`, original kept until verified | Done | tst_ufs2_jobs; Edit image page | Not exercised from the UI end to end |
| Convert exFAT↔ffpkg, image→.ffpfsc | `image.convert` (extract + build + verify) | Done | tst_integration (exFAT→FFPKG→FFPFSC→exFAT byte-identical); "Convert to…" card on Edit image | Not exercised from the UI end to end |
| ffpkg hex preview, compare two images | `image.compare` {a, b, depth}: `integration::compareTrees` over `integration::ImageFiles` (native readers for .exfat / .ffpkg / .ffpfs / .ffpfsc incl. nested containers, or a folder); only in A / only in B (files and folders), different size, different content with the first differing offset (full depth streams both sides), PFSC block padding tolerated; capped lists + exact counts in `JobOutcome::details`, job succeeds whatever it finds. Compare card + result sheet (Copy report) on Extract & verify. Hex preview: `ImageFiles::readSlice` → `services/HexPreview` (worker thread), Edit image › Preview… / double-click, offset/hex/ASCII, 64 KiB + Load more | Done | tst_image_compare (identical exFAT/FFPKG/FFPFSC/dump pairwise, edited copy → every difference kind, structure vs full, PFSC padding with a real compressed .ffpfs, capped lists, cancellation, validation, byte slices from every format); tst_image_tools (hex service on built .exfat/.ffpkg, Preview sheet with Load more, Compare card → queue → result sheet) | Legacy report's UFS2 mode/BSD-flag differences not compared (the readers do not expose them for every format); the preview opens what the editor opens (.exfat/.ffpkg) |

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
| Unity/Il2Cpp module-name case (detector) | `ModuleImports` → plan / library-scan issue `ModuleNameCase` | Done · field check | tst_backport_detect | Dynamic-tag layout on real PS5 titles unverified; the fix stays blocked |
| Audio-fix scanner (legacy hidden tab) | `AudioScan`, `backport.audioScan` (read-only report) | Done | tst_backport_detect | Patch profiles need console evidence (no test patch) |
| User-supplied AMPR/PlayGo libraries with hash pinning | `ProviderSet`, `backport.amprPlaygoFolder`, `backport.pinProviders` | Done · field check | tst_backport_providers | Nothing bundled; whether a given provider loads is the user's test |
| Console logs from the Backport page (payload, kernel log) | Links to the PS5 connect page tabs | Done | — (UI link; screenshot `backport-report`) | Crash-dump download: console paths not documented in this repo |
| Import-table audit, fix families, audio gate and audio patches, Il2Cpp case fix, PS5 Pro fix/Pro build, right.sprx, default fakelib set, bundled AMPR/PlayGo, save-data fixes, SDK 11–13 | — | Blocked | — | Backport GUI source (D:\) not reachable; firmware tables and Sony-derived files have no documented licence/provenance |

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
| Translations (16 languages) | `app/i18n` (`JsonTranslator` over `:/i18n/<code>.json`, English text as key; settings localised at display time; `general.language` + `--language`; Arabic right-to-left), legacy `language` key imported | Done · unreviewed | tst_i18n (files, placeholders, first-screen set, fallback, German and Arabic main window), screenshots in de/ja/ar | Machine-translated (1,710 of 1,767 strings per language; the rest are identifiers), not reviewed by native speakers; engine messages stay English; restart to switch. See `docs/I18N.md` |
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
| Real PC acceptance test (owner's machine) | — | Pending | No Windows PC was connected to this session; see RELEASE_NOTES "Before release" |
