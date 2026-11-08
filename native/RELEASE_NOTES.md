# exFAT Image Builder 5.0.0 — Farewell Edition (native) · release notes

**Status: pre-release.** Built and tested in CI on Windows (MSVC 2022,
Qt 6.8) and Linux; published as a GitHub *pre-release*, so the legacy app's
update check does not offer it. Not yet run on the owner's PC, a PS5 console
or with real Sony Publishing Tools — see *Before release* below. It becomes the
"latest" release only after those checks.

**Download `exFAT Image Builder.exe`** (GitHub lists it as
`exFAT.Image.Builder.exe`) and run it: one file, nothing to unzip or install,
no administrator rights, no VC++ Redistributable. Its first start unpacks the
program once to `%LOCALAPPDATA%\kerrdec97\exFAT Image Builder\runtime`. The
installer and the portable zip are still there for those who prefer them.

## What it is

A complete rebuild of the exFAT Image Builder as a native C++20 / Qt 6
Windows application. Every engine the old app borrowed from outside tools is
now built in, so the new app needs **no administrator rights, no OSFMount, no
.NET runtime and no Python**.

## Highlights

* **Native image engines.**
  * exFAT images are written directly from the Microsoft specification (the
    legacy layout rules: label, cluster and size rules kept).
  * FFPKG (UFS2) images are byte-for-byte identical to UFS2Tool 4.0.0 — and
    keep non-ASCII file names, which UFS2Tool turned into `?`.
  * FFPFSC/FFPFS images are byte-for-byte identical to Lazy_MkPFS, built with
    a multithreaded, deterministic compressor.
* **Verified or it isn't done.** Every build is read back and compared before
  it gets its final name; a failed verification fails the job. Images can
  also be verified on their own, extracted, edited (exFAT in place with an
  undo journal; FFPKG by safe rebuild) and converted between formats.
* **FPKG packages** with your own Publishing Tools: every useful setting from
  PSVIETHOA FPKG Builder 3.0.0, the three PlayGo layouts, update and add-on
  packages, building straight from `.exfat`/`.ffpkg`/`.ffpfsc`/`.gp5`, live
  CPU limit, ASCII-path aliases for non-ASCII folders.
* **Backport** rebuilt with 38 safety rules: nothing in the game folder
  changes until the backport and originals ZIPs are written and read back;
  applying is journaled and rolls back after a crash; restore removes what the
  backport added. Apply now asks before it changes game folders, checks every
  file before the first write and flushes each file to the drive.
* **From the PS5 Backport GUI engine** (ported from its own source): target
  SDK 11–13; the PS5 Pro fix's shader-assert part (opt-in, `… Pro fix backport
  files.zip`); the CRI Unity no-sound fix (12 → 8 channels, on by default);
  the Unity module-name case fix (MGS4, issue #2); an unconvertible
  `right.sprx` replaced by your own standard copy (hash-checked, nothing
  bundled); folders that already hold the canonical fakelib set are refused.
* **Integrated queue** with concurrency, pause, cancel (stops whole process
  trees), retry, automatic retries for transient errors, restart recovery,
  history with CSV export, keep-awake, and an optional sleep/shut-down
  countdown when the queue finishes.
* **PS5 connection**: FTP with parallel transfers and resume, payload sender,
  kernel log, Y2JB tools with backups and read-back checks,
  ShadowMount+/MicroMount configuration, optional upload of every verified
  image; "Find my PS5" network search, FTP diagnostics with a 16 MiB speed
  test, and a setup checklist on Home. Optional Discord message when the
  queue finishes (Settings › General, advanced).
* **Tools**: Language Stripper (always backs up first, restorable), DLC-emu
  editor, library scan and dump rename with undo, problem reports with your
  user name hidden, update checker that only installs SHA-256-verified
  releases.
* **New interface** on the Farewell design system: one accent colour, real
  focus rings, every setting generated from the engines that use it (no
  decorative controls), per-job overrides with clear badges, structured logs.
  Pages: Home, Build image (exFAT, FFPKG, PFS and FPKG on one page, each
  showing only its own options), Extract & verify, Edit image (stage changes,
  apply in one verified job, convert between formats), Backport, Queue, PS5
  connect (FTP browser, payloads, kernel log, Y2JB, mount config), Library
  (scan, covers, dump rename with undo, DLC-emu editor), Logs, Settings, About.
* **Built to be safe with your files.** The queue never runs two jobs on the
  same image or game folder at once; deleting temporary folders or a source
  never follows a junction or symbolic link into another folder; images are
  flushed to the drive before they replace anything; an interrupted exFAT
  edit is undone from its journal; only one copy of the app runs at a time.

## Changed from the legacy app

* Settings import from `~/.exfat_builder_settings.json` (Settings ›
  Advanced). Keys that no longer apply (OSFMount, robocopy, Python back-ends)
  are not imported.
* PFS verification now follows the app-wide Verification settings.
* exFAT "Automatic" cluster size is always 64 KiB, which ShadowMount+ 1.7 and
  later need (the legacy rule picked 32 KiB for games with many small files).
  The other sizes stay available for other loaders.
* Image names: the Build page proposes "PPSA01342 Demon's Souls
  (01.000.000)" (Settings › Build › Output name: Title ID / title / version /
  firmware presets, the source folder's name, or a custom template with
  %TITLE% %PPSA% %VERSION% %FIRMWARE% %SDK% %TYPE% %DATE% %FOLDER%), keeps
  letters such as "ō" and can be edited for each job. The legacy
  `name_preset` and `name_custom_template` settings are imported.
* The Build page finds the game when you choose a folder that holds it one
  or more levels down, and offers a choice when there are several games.
* PFS images can also be built uncompressed (.ffpfs).
* SDK and required firmware versions are read correctly (SDK 10 no longer
  shows as 16).
* Fixed while porting: the Language Stripper crashed on its first match; the
  legacy PFS tab double-compressed `.ffpfsc`; DLC-emu and dump-rename edge
  cases; several data-safety issues listed in `docs/BACKPORT_PARITY.md`.

## Performance

Measured on the build machine (not your PC): exFAT images are written at the
speed of the disk, FFPKG builds match or beat UFS2Tool, and FFPFSC builds are
faster than Lazy_MkPFS at the default level with byte-identical output. See
`docs/PERFORMANCE.md`.

## How it was tested

* **Automated tests** (39 test programs, run on every change): Linux with
  AddressSanitizer/UBSan and Windows (MSVC 2022, Qt 6.8) in CI. They include
  byte-for-byte comparisons with UFS2Tool and Lazy_MkPFS output, the legacy
  app's own JSON/INI/GP5 output as goldens, crash and cancel recovery, and —
  on Windows — mounting the app's exFAT images with the real Windows exFAT
  driver (read back, `chkdsk`) and reading Windows-formatted images natively.
* **The real executable** is built, deployed with its Qt runtime, self-tested,
  made to render every page and kept running for 30 s on a Windows runner.
* **Independent review**: four reviewers read the core, image engines,
  PFS/FPKG and Backport/PS5 code against the specs and with crafted inputs;
  their findings were fixed with regression tests.
* **Evidence for this build** (commit `b29d7b5`, GitHub Actions run
  41804192710 on Windows Server 2025): 36/36 test programs passed; the real
  Windows exFAT driver test passed (mount, read back, `chkdsk`); the deployed
  exe's self-test listed 119 settings and every job kind; it rendered all 41
  page screenshots; it was still running after 30 s (≈ 94 MB working set);
  the installer and portable zip were built with `SHA256SUMS.txt`
  (artifacts `native-windows-packages`, `native-windows-evidence`,
  `native-windows-symbols`). Linux with sanitizers: run 41804192621 passed.
* **Not tested**: your PC (no computer was connected to the session that
  built this), a PS5 console, real Sony Publishing Tools, a real game dump.
  These are the owner's checklist below.

## Known limitations

* Images and packages from untrusted sources are checked as they are read
  (names that would leave the output folder, sizes beyond the file, damaged
  tables are refused), but the app is not a sandbox: only open images you
  trust.
* Y2JB writes a new copy of the console's database and renames it over the
  old one (the legacy app overwrote it in place). Whether a running system
  process keeps the old copy until the console restarts is unverified.
* When Windows refuses to move a locked file aside (for example an old
  package still open in another program), Qt first tries a full copy before
  giving up, so the error can take a while for a very large file. Nothing is
  lost: the new package is kept under a numbered name.
* A file you created yourself named exactly like an output plus `.part`
  (for example `Game.exfat.part`) is overwritten by a build of that output.

## Not in this release

* **Blocked**: the firmware import-table audit and the audio firmware gate
  (the firmware tables need a provenance note; the gate needs an x86
  decoder), the Backport GUI fix families that find their sites by
  disassembly (save-data, mouse, Wwise, GamePresets, Set3DLatency), the PS5
  Pro build / PSSR 2 test and the AGCDriver hook, and any bundled
  Sony-derived file (fakelib set, AMPR/PlayGo providers, `right.sprx`).
* **Later**: crash-dump download and hang snapshots on the PS5 connect page
  (fully specified from the PS5 Log Sender; needs a console to test).
* **Not ported**: interface translations (only English for now), Dokan
  "mount as drive", the zftpd web-UI (ZHTTP) probe of the FTP diagnostics.
  (The autoloader editor and the AMPR index over FTP are on the PS5 connect
  page.)

## Before release (owner's checklist)

Nothing below can be done from the cloud session that built this edition.

1. Install the CI build (`native-windows-packages` artifact) or the portable
   zip on your PC; start it; check Settings › Tools shows every component.
2. Import your old settings (Settings › Advanced) and review them.
3. Build one small game as exFAT, FFPKG and FFPFSC; confirm each passes
   verification; mount/launch each on the PS5.
4. Build one FPKG with your Publishing Tools (small game, then a
   multi-language one); install on the console.
5. Backport one game to a lower SDK; launch it on the target firmware;
   restore it from the originals ZIP.
6. Connect to the PS5 (Find my PS5, FTP browse, upload, payload, kernel log,
   Diagnostics with the speed test).
7. Only then: publish a release (`v5.0.0`) with the installer, portable zip
   and `SHA256SUMS.txt` (the update checker requires the checksum file).

## Requirements

Windows 10 1809 or later, 64-bit. About 120 MB of disk for the app. Sony
Publishing Tools only for FPKG packages (user-supplied).

## Licence

GPL-3.0-or-later; Qt under LGPL-3.0. Third-party notices ship with the app.
