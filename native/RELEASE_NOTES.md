# exFAT Image Builder 5.0.0 · release notes

**Download `exFAT Image Builder.exe`** (GitHub lists it as
`exFAT.Image.Builder.exe`) and run it: one file, nothing to unzip or install,
no administrator rights, no OSFMount, no .NET, no Python, no VC++
Redistributable. The first start unpacks the program once to
`%LOCALAPPDATA%\kerrdec97\exFAT Image Builder\runtime` (with `portable.txt`
next to the exe: beside the exe instead). Check it against `SHA256SUMS.txt`.

**Status: pre-release** until it has been run on a PS5 console (checklist at
the end).

## What's new

Version 5 is a complete rewrite of the app as a native C++20 / Qt 6 Windows
program, with a new interface and the PS5 Backport engine built in.

### A new interface
* A new design: blue-black surfaces, one signature gradient for what matters
  (the main action, selection, progress), big clear targets, and far less
  text. Help is one short line; the details are a hover away. Rare settings
  sit in a folded **Advanced options** section on each page.
* **Home** is a real landing page: what to do first, console / queue / disk
  at a glance, the last problem with Retry, recent jobs and a PS5 setup guide
  that disappears once done.
* Fewer, clearer pages: **Build image** · **Image tools** · **Backport** ·
  Queue · PS5 connect · Library · Logs · Settings · About.

### Build image
* Drop a dump folder (or the folder above it: the game inside is found for
  you), pick **exFAT**, **FFPKG**, **PFS** or **FPKG**, done. Each format
  shows only its own options.
* **Zipped dumps**: drop a .zip of a game and build exFAT, FFPKG or PFS from
  it directly; it is unpacked to a temporary folder and checked as it goes.
* **FPKG** packages moved onto the Build page (built with your own Sony
  Publishing Tools; app, add-on and update packages; from a folder, an image
  or a .gp5 project).
* **PFS**: compressed `.ffpfsc` or uncompressed `.ffpfs`.
* File names: editable, Unicode kept ("Ghost of Yōtei"), presets and a
  template with `%TITLE% %PPSA% %VERSION% %FIRMWARE% %TYPE% %DATE% %SDK%
  %FOLDER%` (Settings › Build › Output name), imported from v4 settings.
* exFAT images use **64 KB clusters** by default (ShadowMount+ 1.7 requires
  them).
* Every image is read back and verified before it gets its final name.

### Backport — the PS5 Backport engine, built in
* The Backport tab runs the **PS5 Backport engine 0.3.4 by Deckerr97** — the
  same engine as the standalone PS5 Backport GUI, bundled unchanged — so
  results are identical: Target SDK 4–13, PS5 Pro fix, PS5 Pro build,
  Rebuild a ZIP with another SDK, Switch AMPR / PlayGo (known-good libraries
  or your own signed .sprx), Apply to the game folder with a verified
  originals backup, Restore originals, interrupted-apply recovery, Export logs
  and Report a problem.
* It shares the standalone program's settings and applied-backports record,
  so your scan folders, destinations and applied backports carry over.
* The old built-in backport port is gone; the engine replaces it.

### Image tools (was Extract & verify + Edit image)
* Open an image once, then **Extract**, **Verify** (its own checksums, or
  compare with a folder or another image), **Convert** (exFAT, FFPKG, FFPFSC,
  FFPFS; exFAT/FFPKG → FFPFSC packs the image as it is, without a temporary
  copy) or **Edit** (add, replace, rename, delete).

### Library
* Cover-art grid or list, format chips per game, search, filter, sort,
  multi-select with bulk Build / Backport.
* New **Images** tab: your .exfat/.ffpkg/.ffpfs/.ffpfsc files with their
  title read from inside the image, rename by title/ID/version, open in Image
  tools.

### PS5 connect
* A console card with live service chips (FTP server and version, kernel
  log, payload loader, ShadowMount+ version, package installer) and five
  clear groups: **Files**, **Payloads**, **Logs**, **ShadowMount+** and
  **Console tools**.
* A real **file manager**: places (games, USB, extended storage, M.2,
  ShadowMount+, etaHEN), breadcrumbs, sortable list, multi-select, drag and
  drop, keyboard shortcuts, and a transfers panel with per-file progress,
  speed, time left, pause, cancel and retry.
* **ShadowMount+ 1.7**: reads and writes the 1.6/1.7 flat and the new
  sectioned format (, ), changes only the keys you
  edit (comments and unknown keys are kept), drops the removed 
  keys, uses its HTTP API (port 10101) for version, free space, USB eject and
  logs, and confirms **Safe to unplug** from its own shutdown messages.
* **ftpsrv 0.21**: SELF decryption is switched off after login, so uploads
  and backups of fake-signed files keep their exact bytes.
* Payloads default to **port 9021** (elfldr); a stored 9090 (etaHEN's package
  installer) is moved once. Package install only reports success when the
  console says so. Autoloader rules follow the current format; the AMPR index
  refuses folders that use ampr_emu 0.4.2.1 asset packs; Find my PS5 also
  probes ports 1337 and 2120; Y2JB shows the firmware → YouTube version table;
  crash-dump capture saves each console crash as a ZIP.

### Single-file download
* One exe with a small native launcher (no VC++ runtime needed); cached start
  ~70 ms; the payload is checked with SHA-256 before it runs; portable mode
  with `portable.txt`.

## GitHub issues fixed
Fixed or made obsolete by the rewrite: #86, #85, #84, #82, #81, #80, #79,
#77, #76, #74, #73, #72, #71, #69, #68, #65, #61, #60, #53, #43, #39, #36,
#33, #27, #21, #20, #19, #67 (ZIP dumps; #37 asked to stream without
unpacking: ZIPs are unpacked to a temporary folder) and PR #57 superseded.
Not in this release: 7z/RAR archives, interface translations (#35, #23), a
dump checker (#26).

## How it was tested
* **Automated tests** (45 test programs) on Windows (MSVC 2022, Qt 6.10):
  every engine, byte-for-byte golden comparisons with UFS2Tool and
  Lazy_MkPFS, every page opening, the Build page submitting every format,
  Image tools jobs, the Library, the PS5 tools against a fake console, the
  single-file launcher (arguments, exit codes, caches, damaged payloads,
  concurrent first starts, portable mode).
* **Real end-to-end runs on this PC**: exFAT, FFPKG, FFPFSC, FFPFS and FPKG
  (fake Publishing Tools) builds of a dummy dump with a Unicode name, all
  verified; extract / verify / convert / edit of those images; a **real game
  backport** (Ghost Song, PPSA09232) through the Backport tab — clicking
  Start Backport, the bundled engine, the backport and originals ZIPs — plus
  an AMPR/PlayGo switch, an SDK 5 rebuild and a cancel of the same game.
* **Not tested here**: a PS5 console (mounting, payloads, ShadowMount+,
  launching a backport on its target firmware) and real Sony Publishing
  Tools.

## Known limitations
* Not code-signed; some antivirus engines flag unsigned self-extracting
  programs by heuristics. Check the SHA-256, or build it yourself.
* English only for now.
* Images from untrusted sources are checked as they are read, but the app is
  not a sandbox: open only images you trust.

## Before marking it "latest" (owner's checklist)
1. Build one small game as exFAT, FFPKG and FFPFSC; mount/launch each on the
   PS5 with ShadowMount+ 1.7.
2. Backport one game; launch it on the target firmware; restore it.
3. PS5 connect: connect, browse/upload/download over FTP, send a payload,
   kernel log, ShadowMount+ config load/save, Safe to unplug.
4. One FPKG with your Publishing Tools; install it.

## Requirements
Windows 10 1809+ or 11, 64-bit.

## Licence
GPL-3.0-or-later. The bundled PS5 Backport engine is a separate program
under its own terms (see THIRD_PARTY_NOTICES.md).
