# exFAT Image Builder 5.0.0 — Farewell Edition (native) · release notes

**Status: release candidate, not published.** Built and tested in CI on
Windows (MSVC 2022, Qt 6.8) and Linux; not yet run on the owner's PC, a PS5
console or with real Sony Publishing Tools. See *Before release* below.

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
  backport added.
* **Integrated queue** with concurrency, pause, cancel (stops whole process
  trees), retry, automatic retries for transient errors, restart recovery,
  history with CSV export, keep-awake, and an optional sleep/shut-down
  countdown when the queue finishes.
* **PS5 connection**: FTP with parallel transfers and resume, payload sender,
  kernel log, Y2JB tools with backups and read-back checks,
  ShadowMount+/MicroMount configuration, optional upload of every verified
  image.
* **Tools**: Language Stripper (always backs up first, restorable), DLC-emu
  editor, library scan and dump rename with undo, problem reports with your
  user name hidden, update checker that only installs SHA-256-verified
  releases.
* **New interface** on the Farewell design system: one accent colour, real
  focus rings, every setting generated from the engines that use it (no
  decorative controls), per-job overrides with clear badges, structured logs.

## Changed from the legacy app

* Settings import from `~/.exfat_builder_settings.json` (Settings ›
  Advanced). Keys that no longer apply (OSFMount, robocopy, Python back-ends)
  are not imported.
* PFS verification now follows the app-wide Verification settings.
* exFAT "Auto" cluster size is described as it really behaves (32 KiB for
  small-file games, 64 KiB otherwise); the legacy label said "Auto (64K)".
* SDK versions from `param.sfo` are read correctly (SDK 10 no longer shows as
  16).
* Fixed while porting: the Language Stripper crashed on its first match; the
  legacy PFS tab double-compressed `.ffpfsc`; DLC-emu and dump-rename edge
  cases; several data-safety issues listed in `docs/BACKPORT_PARITY.md`.

## Not in this release

* **Blocked** (licence or provenance, or the Backport GUI source on the
  owner's D:\ drive): firmware import-table audit, fix families, audio gate,
  PS5 Pro fix/Pro build, `right.sprx` replacement, default fakelib set,
  save-data fixes, SDK pairs 11–13.
* **Not ported**: interface translations (only English for now), Dokan
  "mount as drive", FTP speed test and IP sweep, autoloader editor, AMPR index
  over FTP, Discord webhook.

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
6. Connect to the PS5 (FTP browse, upload, payload, kernel log).
7. Only then: publish a release (`v5.0.0`) with the installer, portable zip
   and `SHA256SUMS.txt` (the update checker requires the checksum file).

## Requirements

Windows 10 1809 or later, 64-bit. About 120 MB of disk for the app. Sony
Publishing Tools only for FPKG packages (user-supplied).

## Licence

GPL-3.0-or-later; Qt under LGPL-3.0. Third-party notices ship with the app.
