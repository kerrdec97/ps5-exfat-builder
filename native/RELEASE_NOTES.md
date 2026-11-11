# exFAT Image Builder 5.0.2 · release notes

**Download `exFAT Image Builder.exe`** (GitHub lists it as
`exFAT.Image.Builder.exe`) and run it: one file, nothing to unzip or install,
no administrator rights, no OSFMount, no .NET, no Python, no VC++
Redistributable. The first start unpacks the program once to
`%LOCALAPPDATA%\kerrdec97\exFAT Image Builder\runtime` (with `portable.txt`
next to the exe: beside the exe instead). Check it against `SHA256SUMS.txt`.

Antivirus removes the exe? Download the **folder edition**
(`exFAT-Image-Builder-5.0.2-win64-folder.zip`) instead: the same program,
already unpacked. Extract it anywhere and run the exe inside.

## What's new in 5.0.2

Every part of the app was read line by line (engines, queue, launcher,
console code, every page, the build scripts and the tests); this version
carries the fixes, a design refresh and answers to the open GitHub issues.
Images built with 5.0.1 stay valid.

### From your reports (GitHub issues)
* **`ampr_emu.index` is back for exFAT and FFPKG builds** (#93). A game that
  uses the AMPR emulator (`fakelib/libSceAmpr.sprx`) gets the index when the
  dump has none, and a fresh one when it no longer matches the game's files,
  as version 4 did. The index goes into the image only; the dump folder is
  never changed. New setting **AMPR file index** (Settings › Build, and under
  Advanced on the Build page): when the game uses the AMPR emulator
  (default), add when the dump has none, always rebuild, never add or change.
  PFS builds have the same new choice. The build log says what was done.
* **Add to queue makes a list** (#87). It puts the job on the list instead of
  starting it: add as many games as you like, then press **Start queue** (on
  the message, in the Jobs panel or on the Queue page). The main button
  ("Build image", "Pack game", …) still starts that one job at once. A waiting
  list is kept when the app is closed.
* **7z and RAR archives as build sources** (#37), as ZIPs already were:
  choose or drop `game.7z` or `game.rar`. For a multi-part RAR, choose the
  first part. Every file's checksum is checked while it is unpacked.
  Password-protected archives, solid RAR 4 archives and 7z archives using
  BZip2 are refused with a plain explanation.
* **Check dump** (#26): Library › Games › "Check dump…" reads a dump without
  changing it and lists missing or empty files, encrypted or cut-short
  executables, signs of a backport, junk files, links and names exFAT cannot
  store, with a report you can copy.
* **Where the app keeps its files** (#91, #90): Settings › Storage lists the
  unpacked program, settings, queue, logs and temporary files, each with
  "Show in folder". "Make this copy portable…" moves them next to the exe,
  for example on an external drive, copying your settings and queue; the
  switch happens at the next start.
* **Renaming images is easy to find** (#88): Library › Images has a "Rename
  images…" button, and a "Rename…" button when images are selected. Names can
  be "PPSA01234 - Title" or follow your own template, with a live preview.
* **Folder edition** (#78): each release also offers the program as a zip,
  already unpacked, for PCs whose antivirus removes the single exe.
* **Languages are back** (#35, #89): Settings › General › Language offers 42
  languages next to English: every language Windows 11 itself ships in, plus
  Persian, Urdu, Hindi, Bengali, Malay and Filipino. Arabic, Persian, Hebrew
  and Urdu mirror the whole window (right to left); paths, IDs and logs stay
  left to right. The interface switches after a restart ("Restart now"). Log
  files and error details from the engines stay in English. The translations
  are machine-made; "Show in folder" next to the setting opens a folder where
  you can correct a translation or add a language yourself (README.txt there
  says how).

### Safety of your files
* **Unpacking a crafted packed game or image can no longer write outside the
  folder you chose** (Unpack, Extract, Pack with zstd, FPKG from an image).
  Such names are refused before anything is written.
* **Pack and Unpack never delete anything they did not create**: they work in
  `<output>.part` and rename it when done. A Packed folder no longer shares
  its loose files with the original dump through hard links.
* **A finished, verified image is never thrown away** when the file it should
  replace is open elsewhere (mounted, being uploaded): it is saved next to it
  as "<name> (new)" and the message says where.
* The clean-up at start no longer removes your own folders named `fw-…` from a
  temporary folder you chose in Settings; only the app's own job folders go.
* Unpacking a ZIP fails with a clear message when the drive fills up, instead
  of leaving empty files that the image was then built from.
* Converting an image keeps every file, whatever "Leave out" patterns are set.
* Removing and restoring languages touch only files that are safely in the
  checked backup; Dump Rename applies only what the preview showed and says
  exactly what a cancelled rename or undo did.
* A game folder the app is not allowed to read now stops the build instead of
  producing an image without those files.

### Damaged and crafted files
* A damaged or deliberately broken `.exfat`, `.ffpkg` or PFS image, `.pkg`,
  `param.sfo`, cover or `eboot.bin` can no longer hang the app or use up all
  memory; it is reported as damaged within moments.
* Titles, file names and messages that come from dumps, images, packages, tools
  or a console are always shown as plain text, never as formatting.
* Problem reports no longer contain passcodes, the DLC entitlement key, tokens
  or webhook addresses; a console address is kept only when it is a local
  network address. Crash reports (text) are now really included.

### Updates
* **"Check for updates" now finds version 5 releases** (5.0.0 and 5.0.1 said
  "Up to date" for every release). When a version is available the app
  downloads it from GitHub on request, checks it against the release's SHA-256
  list and shows where the file is; you replace the program file yourself.
  There is no installer any more and the running program is never replaced.

### Backport
* A run can no longer be started twice, and a scan that finishes during a run
  no longer makes the page forget the run.
* **The settings shared with the standalone PS5 Backport GUI are no longer
  overwritten with an old copy**: a change made in the other program is kept,
  and a damaged or locked settings file is left alone.
* An apply still running in the standalone program is no longer offered as
  "interrupted". "Clear list" asks first; "Switch AMPR / PlayGo" no longer
  crashes when both lists are left on "Keep".

### Build image and FPKG
* **Pause really pauses an FPKG build**: Sony's publisher is suspended until
  you resume. Performance mode and the CPU limit also apply to package
  verification, extraction and backport builds.
* "Overwrite" keeps your previous package until the new one passed its checks.
* Importing old settings no longer limits builds to one CPU thread.
* The free-space check is repeated when you press the button, so freeing space
  or plugging the drive in is enough. A Packed folder can be saved to a FAT32
  drive again; files of 4 GB or more are refused up front on FAT32 for every
  image format.
* An image or `.gp5` source no longer inherits the content ID and version of
  the game chosen before; the Content ID field is read-only except for add-ons.
* Images built with zstd packs are uploaded to the PS5 when "Upload new images
  to the PS5" is on. zstd builds include folders that are links or junctions.
* Zipped dumps: stage names stay right after "Unpacking ZIP", the image is
  named without ".zip", and a ZIP is never deleted by "delete the source".

### Image tools, Library, DLC
* Image tools reads the file tree again after an edit, keeps staged changes on
  F5 and asks before discarding them; Extract waits until the image was read.
* Editing an exFAT image in place shows progress, can be cancelled, and a
  finished edit can no longer be undone later by a leftover undo file.
* Library: a rescan keeps dumps you added by hand; renaming several images
  keeps their extension; games that carry a backport show a badge.
* DLC and dlc_emu.ini: deleting a row is safe, unsaved edits are asked about,
  keys stay with their entry, and one backup copy is kept instead of many.

### PS5 connect
* Fixed a crash when a folder listing changed under the right-click menu.
* Disconnect really stops (queued deletes no longer run after it); Rename
  refuses a name that already exists; cancelling a transfer, dropping files on
  the page and uploading over a file ask first.
* `/user`, `/mnt` and folders named like system folders get the same extra
  confirmation as the system folders themselves.
* A payload or package the console stopped accepting part-way is reported as
  failed instead of "sent"; a transfer that gives up no longer leaves a
  half-written file behind without saying so.
* Pushing ShadowMount+ settings keeps comments, line endings and unusual
  characters of the lines it does not change. "Safe to unplug" asks before it
  stops ShadowMount+. "Find my PS5" only searches private home networks.

### Interface: a design refresh
* **Depth and contrast**: cards lift off the page, fields read as wells, lines
  and secondary text are easier to see, and buttons no longer look like fields.
  White text on the blue-violet gradient and on red buttons now passes the
  4.5:1 contrast mark.
* **The page title and its main button stay in view** while a long form
  scrolls, so "Build image" or "Start Backport" is always one click away. The
  button's tooltip names the first thing that is still missing.
* **Motion**: switches, segmented controls, hover states, progress bars, toasts
  and dialogs now move, briefly and quietly. Windows' "show animations" setting
  turns it all off.
* **Games without cover art** get one of six colours from their title ID
  instead of identical blocks; dropdowns show their arrow; empty lists are
  compact and say what to do next.
* **Job monitor**: larger percentage, clearer status line, a tinted result
  block for success and failure.
* Backport: status chips never cut their text, the selection links sit in one
  group, shorter folder placeholders. Build: the two backport folders each get
  the full width.
* Text colours match the design again: links are blue, captions are dimmer than
  body text, errors red, confirmations green, and the separator lines are back.
* Scrolling a page no longer changes a dropdown or number field under the
  pointer. Enter presses the focused button and a dialog's default button.
* Retry, Run again and Show in folder say why nothing happened.
* The Queue shows the time left for the whole job, lists every job that runs
  at the same time, and its "Failed" tab is now "Not finished".
* New app icon in all sizes. The Logs page no longer loses lines.
* Settings: a description that just wraps onto a second line is no longer cut
  off (Storage › Default output folder).
* "When the queue finishes": with "Only if every job succeeded" off the action
  also runs after failures.

### Under the hood
* The program file no longer contains the folder path of the PC it was built
  on. The launcher loads Windows libraries only from the Windows folder.
* 73 automated test programs (53 in 5.0.1), among them crafted-image, 7z and RAR,
  path-traversal and shared-settings tests. Not tested: a PS5 console, real
  Sony tools, a real GitHub release.

## What version 5 brought (5.0.0 and 5.0.1)

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
  **DLC** · Queue · PS5 connect · Library · Logs · Settings · About.

### Watching a job: the monitor
* Press **Build image** (or Extract, Convert, Convert DLC…) and the page
  turns into that job's monitor: a big progress bar with the percentage, the
  stage it is in, speed, time left and the file being written; five live
  numbers (elapsed, remaining, speed with a small graph, done, stage); the
  **stage timeline** with the time each stage took; the **live log** — every
  line the job and the Sony tools write, as it happens, warnings and errors
  marked, a Verbose switch for the tools' own output, Copy and Open in Logs;
  the result (verification, warnings, outputs) and the job's buttons (Pause,
  Cancel, Show in folder, Retry / Run again, New build).
* **Add to queue** keeps the form; **Watch** on the Jobs dock opens any
  running job in the monitor; the Queue page shows the same monitor for the
  selected job.
* A cancelled job now shows **Cancelling…** until it has stopped at its safe
  point (before, a paused job that was cancelled kept saying "Paused").
* **Build now / Retry / Run again start that one job**: a queue you paused
  stays paused for everything else. Update checks and problem reports also
  no longer resume a paused queue.

### Build image
* Drop a dump folder (or the folder above it: the game inside is found for
  you), pick **exFAT**, **FFPKG**, **PFS**, **FPKG** or **Packed folder**,
  done. Each output shows only its own options.
* **Zipped dumps**: drop a .zip of a game and build exFAT, FFPKG or PFS from
  it directly; it is unpacked to a temporary folder and checked as it goes.
* **FPKG** packages moved onto the Build page (built with your own Sony
  Publishing Tools; app, add-on and update packages; from a folder, an image
  or a .gp5 project).
* **PFS**: compressed `.ffpfsc` or uncompressed `.ffpfs` (how much
  `.ffpfsc` saves depends on the game; it is no longer called "smallest").
* **Performance mode on every format**: Low impact / Balanced / Maximum right
  on the Build page, with what it means on this PC ("16 logical cores:
  Maximum uses 16 …"); CPU threads per job under Advanced. A £300 laptop can
  build without freezing, a workstation can use every core.
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

### Backport updates — backports as their own installable package
From the owner's ppr-pkg Companion ("DualBackports"):
* **Build image › FPKG › Backport**: choose the base package installed on
  the console and the 4.xx and/or 5.xx+ backport files (a folder, or the ZIP
  the Backport tab made). Each backport becomes its own small update
  (`<TITLEID>-backport-4xx.pkg`, `<TITLEID>-backport-5xxplus.pkg`) that
  installs over the base, so the base stays the original game. The game
  folder is optional (without it the base is unpacked); the new version is
  filled in from the base (last field + 1).
* **FPKG › App** with backport files builds the game's package first and
  then the separate updates against it in one job.
* Right-click a backported game on the Backport tab › **Package as an
  installable update** opens this with everything filled in.
* Every update is checked before it gets its name: the base passes Sony's
  integrity check, the update must be a delta that references the base's
  digest (an update that does not fails on the console with CE-107891-6), its
  full image must carry the base's identity and the requested version and
  pass the integrity check. Existing files are never replaced (`-2`, `-3`).
* Needs Sony's publisher from a verified SDK Fix release (sdk-fpkg279-fix13,
  or fix12 with a note); the toolkit is checked by SHA-256.

### DLC packages (new page)
From the ppr-pkg Companion's DLC Converter:
* Drop DLC INIs (dlc_emu.ini and other DLC lists) or DLC .pkg files; edit
  titles and IDs in the list, pick icon0.png, and **Change region…** to
  retarget DLC to another region's game: the publisher prefix and title ID
  change, each DLC keeps its own suffix. The game's regional releases are
  looked up on the PlayStation Store, SerialStation and ProsperoPatches (can
  be turned off; manual identifiers always work).
* **Convert DLC** builds installable PS5 DLC packages (`<ContentId>.pkg`),
  verified and read back; **Save updated INIs** writes edited copies that keep
  comments, unknown keys and the encoding. A results report is saved.
* Built with fpkg-cli (LibProsperoPkg); choose your own copy in Settings ›
  Tools.

### Settings from the Companion
* Performance › **Performance mode** (Low impact / Balanced / Maximum): CPU,
  disk and memory priority of Sony's publisher and the DLC engine, and the
  core count when "CPU threads per job" is 0.
* PlayGo › **All 31 PlayGo languages**.
* FPKG › package size class is the SDK Fix attributePub rule.
* DLC › output folder, entitlement key, PKG source passcode, online lookup,
  report, and the DLC engine options (image mode, Kraken level and encoder,
  PFS format, SDK override, deterministic builds, full verification).

### zstd packs (ampr-emu-ng) — new on the Build page
* **Compress game files (zstd packs)**, a switch on exFAT, FFPKG and PFS: the
  game's files go into `ampr_zstd.index` + `ampr_zstd-NNN.pak` volumes
  (256 KiB zstd chunks, the format of quer3q's ampr-emu-ng) inside the image,
  and the bundled **ampr-emu-ng 0.3.3** runtime goes to
  `fakelib/libSceAmpr.sprx`. A smaller image; mount it with ShadowMount+ with
  "Update game fakelib emulators" off.
* **Packed folder**, the fifth output on the Build page: no image, the game
  folder itself with its files in zstd packs and the runtime in `fakelib`,
  ready for a USB drive or an FTP upload. The same settings (level, what to
  pack, check after packing), from a dump folder or a zipped dump; **Pack
  game** turns the page into the job's monitor like every build. The Library
  has **Pack with zstd…** on a game and **Packed folder** in the bulk Build
  images… sheet. A packed folder next to the dump gets " (zstd)" after its
  name, and the dump folder itself is never the output.
* Advanced: zstd level (3, 5 default, 9, 15, 19), what to pack ("everything
  except system files", or "compatible": containers, configs and media stay
  loose for games that do not start fully packed), check after packing.
* Fixes for problems reported against ps5-dump-forge and ampr-emu-ng:
  * names keep their exact case in both indexes (Silksong crashed on a
    case mismatch);
  * only `fakelib/libSceAmpr.sprx` is replaced, never the game's other fakelib
    libraries (backports keep working), and the log says what was there;
  * `ampr_emu.index` is rebuilt from the game's real file list, the old AMPR
    journal and logs are left out;
  * no empty folder is created where a folder holds only packed files (the
    runtime would list the empty folder instead of the packed one);
  * every chunk is decoded again while packing and every volume is read back
    against its CRCs; the output is identical whatever the thread count;
  * a plain (unsigned) eboot.bin is reported before you waste a build;
  * exclusions you set are applied before packing; the work folder sits next
    to the output (loose files hard-linked, no copy) and is removed after the
    build, also after a crash.
* A packed game (zstd, or LZ4 from ampr_emu) is read through its packs
  everywhere: building it again repacks it, FPKG unpacks it.

### FPKG straight from images and packed games
* Build an FPKG from an **.exfat, .ffpkg, .ffpfs or .ffpfsc** image, including
  an image wrapped in an .ffpfsc (ps5-dump-forge's default, or this app's own
  "pack the image as it is"), and from a **zstd- or LZ4-packed** game folder.
* Only the files the package keeps are written: packs are unpacked on the fly
  and SDK files, dump leftovers, the AMPR emulator, its index, journal and
  logs are left out — one copy of the game, not two. It goes next to the
  output (not on the app data drive) and is removed afterwards.

### Image tools (was Extract & verify + Edit image)
* Open an image once, then **Extract**, **Verify** (its own checksums, or
  compare with a folder or another image), **Convert** (exFAT, FFPKG, FFPFSC,
  FFPFS; exFAT/FFPKG → FFPFSC packs the image as it is, without a temporary
  copy) or **Edit** (add, replace, rename, delete).
* An .ffpfsc that holds an .exfat or .ffpkg (ps5-dump-forge, Meaners) opens
  straight through to the game inside: title, extract and FPKG without
  unpacking the inner image first.
* **Extract packed games**: an image or folder with zstd or LZ4 packs shows
  "zstd-packed"; Extract writes the game's own files (switch: keep the packs
  as they are). Every file is read back afterwards.

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
  sectioned format (`version = 2`, `profiles.ini`), changes only the keys you
  edit (comments and unknown keys are kept), drops the removed `kstuff_*`
  keys, uses its HTTP API (port 10101) for version, free space, USB eject and
  logs, and confirms **Safe to unplug** from its own shutdown messages.
* **ftpsrv 0.21**: SELF decryption is switched off after login, so uploads
  and backups of fake-signed files keep their exact bytes.
* Payloads default to **port 9021** (elfldr); a stored 9090 (etaHEN's package
  installer) is moved once. Package install only reports success when the
  console says so. Autoloader rules follow the current format; the AMPR index
  refuses folders that use ampr_emu 0.4.2.1 asset packs; Find my PS5 also
  probes ports 1337 and 2120; Y2JB shows the firmware → YouTube version table
  with the exploit of each range (Y2JB 1.6, Relapse for 13.40–13.60), lists
  the YouTube packages in your own folder with the firmware each one is for,
  and installs the right one in one click;
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
Still open: 7z/RAR archives and streaming from archives (#37), interface
translations (#35), a dump checker (#26), antivirus false positives until the
exe is code-signed (#78).

## How it was tested
* **Automated tests** (63 test programs) on Windows (MSVC 2022, Qt 6.10):
  every engine, byte-for-byte golden comparisons with UFS2Tool and
  Lazy_MkPFS, every page opening, the job monitor following a real job
  (stages, clocks, live log, Run again) and the form → monitor switch,
  the Build page submitting every output and packing a dump into a zstd
  folder (from the folder and from a ZIP),
  Image tools jobs, the Library, the PS5 tools against a fake console, the
  single-file launcher (arguments, exit codes, caches, damaged payloads,
  concurrent first starts, portable mode), zstd packs (format, multi-volume
  packs, identical output at any thread count, read-back through every image
  format, images inside .ffpfsc, FPKG staging, LZ4 reading), backport update
  packages and DLC conversion.
* **Real end-to-end runs on this PC**: exFAT, FFPKG, FFPFSC, FFPFS and FPKG
  (fake Publishing Tools) builds of a dummy dump with a Unicode name, all
  verified; extract / verify / convert / edit of those images; a **real game
  backport** (Ghost Song, PPSA09232) through the Backport tab — clicking
  Start Backport, the bundled engine, the backport and originals ZIPs — plus
  an AMPR/PlayGo switch, an SDK 5 rebuild and a cancel of the same game.
* **With real Sony Publishing Tools and fpkg-cli** (the owner's own copies):
  backport update packages for Ghost Song (base + 4.xx and 5.xx+ updates,
  checked against the base); an FPKG built straight from a zstd-packed exFAT
  image; DLC packages from an INI entry and from a DLC package, retargeted to
  another region.
* **zstd packs with a real dump** (Ghost Song): packed into exFAT, FFPKG and
  FFPFS and read back byte for byte.
* **Not tested here**: a PS5 console (mounting, payloads, ShadowMount+,
  launching a backport on its target firmware, a zstd-packed game, installing
  the packages).

## zstd packs: what to know
* Packed games were tested here by reading them back byte for byte and by
  building a real FPKG from a packed image, not on a console. ampr-emu-ng
  0.3.3 reports for these games are open: Crimson Desert, God of War
  Ragnarök, Sonic X Shadow Generations, Forza Horizon 5, Kingdom Come:
  Deliverance II, Metal Gear Solid 4, Ratchet & Clank: Rift Apart, Wolverine,
  7 Days to Die, Astro Bot. Keep your original dump.
* LZ4 packs are only read (unpacked), never written.

## Reliability and safety fixes from the first engineering audit (5.0.0)
A read of every engine, the queue, the launcher, the console code and the
interface before this release. What changed:
* **Queue**: a job's context menu, drag or cancel confirmation can no longer
  outlive the row it belongs to (a crash when the job ended under an open
  menu); jobs cancelled while waiting and jobs interrupted by a crash are in
  the history and survive a restart until you retry or remove them; the
  "queue finished" notification and the after-queue power action fire when
  the queue finishes even if you paused it, and never for a run you cancelled
  in full; the job history is kept in memory (it was re-read from disk on
  every queue change); the free-space probes no longer run on the interface
  thread (a sleeping network drive froze the window).
* **Interface**: a confirmation sheet now blocks the window's shortcuts,
  drops and Esc; Esc never offers to cancel an update check or a problem
  report; settings of another build of the app (the private one with more
  engines) are kept in settings.json instead of being deleted; search fields
  wait for a pause in typing; progress updates no longer rebuild widgets ten
  times a second.
* **Extraction and files**: "Extract" of a game or image goes to a `.part`
  folder first and is renamed only after it verified (an unverified or
  cancelled extraction never sits under the final name); a hidden
  `desktop.ini` in the output folder no longer throws a finished exFAT
  extraction away; read-only folders left by an extraction can be removed
  again; a source file deleted during a build is reported as such.
* **FPKG / DLC / packs**: an `img_extract` that printed `[Error]` with exit
  code 0 is a failure (a partly extracted base could become a delta that
  deletes files); passcodes are masked in the logs and problem reports; the
  free-space checks before extracting a package ask for twice the package
  size (the tree is larger than the Kraken-compressed file); an image staged
  onto a FAT32 drive is refused before copying; a DLC conversion cancelled
  half-way is reported as cancelled with the packages it did not build; the
  publisher's progress bar can no longer run backwards.
* **PS5 connect**: cancelling an upload against a console that went to rest
  mode returns in seconds (it retried the cleanup for minutes); a listing
  without a size fact no longer breaks downloads; a stalled listing drops the
  connection instead of answering the next command with the old reply;
  Connect, Return in the address field and the chips follow one connection
  state (no double connects, "not connected" after you disconnect instead of
  "not reachable"); downloads of two files whose names differ only in case
  are skipped instead of written over each other; `/system_data`, `/user/app`,
  `/user/home` and `/mnt/sandbox` ask for the extra confirmation; a
  ShadowMount+ `profiles.ini` section whose image name holds `]` is parsed
  right, and a flat-format value with `#` or `;` is refused instead of
  silently truncated on the console; the diagnostics speed test will not
  write into a system folder; Y2JB's small files are swapped in place
  instead of overwritten; an interrupted update download is reported as
  interrupted.

## Known limitations
* Not code-signed; some antivirus engines flag unsigned self-extracting
  programs by heuristics. Check the SHA-256, or use the folder edition.
* Interface languages: English and 42 machine-made translations, not yet
  reviewed by native speakers. Log files and engine error details are in
  English.
* Images from untrusted sources are checked as they are read, but the app is
  not a sandbox: open only images you trust.

## Before marking it "latest" (owner's checklist)
1. Build one small game as exFAT, FFPKG and FFPFSC; mount/launch each on the
   PS5 with ShadowMount+ 1.7.
2. Backport one game; launch it on the target firmware; restore it.
3. PS5 connect: connect, browse/upload/download over FTP, send a payload,
   kernel log, ShadowMount+ config load/save, Safe to unplug.
4. One FPKG with your Publishing Tools; install it.
5. A backport update package over its base, and one DLC package; install them.
6. One game with zstd packs (ShadowMount+, fakelib updates off); launch it and
   check that "ampr-emu-ng: v0.3.3 (zstd) running" shows.
7. One game as a Packed folder on a USB drive (or sent over FTP); launch it
   the same way.

## Requirements
Windows 10 1809+ or 11, 64-bit.

## Licence
Proprietary from this release: Copyright © 2026 Deckerr97 (kerrdec97), all
rights reserved (`LICENCE.txt` next to the app). Third-party components keep
their own licences (`THIRD_PARTY_NOTICES.md`); the bundled PS5 Backport engine
is a separate program under its own terms.
