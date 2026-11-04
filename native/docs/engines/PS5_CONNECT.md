# ps5 — console connection (FTP, payloads, kernel log, DPI, Y2JB, ShadowMount+/MicroMount)

Target `fw_connect_ps5` (`native/src/connect/ps5/`), links Qt Network, **Qt SQL**
(SQLite, only for the Y2JB `app.db`/`appinfo.db` edits) and `fw_engine_gameinfo`.

> **Nothing in this module has been run against a real PS5.** Every test runs
> against in-process fakes written for the tests (`native/tests/connect/ps5/FakePs5.h`).
> Protocol details for the console-side services (ftpsrv / etaHEN FTP, klogsrv,
> the ELF loader, etaHEN DPI V2, ShadowMount+, MicroMount) come from the legacy
> code, not from their sources (inventory U4, U5).

## Headers

| Header | What it gives |
|---|---|
| `FtpClient.h` | `FtpClient` (blocking, one control connection), `FtpOptions`, `FtpReply`, `FtpFeatures`, `ftpReplyError()` |
| `FtpListing.h` | `parseListLine()`, `parseMlsdLine()`, `parseListing()`, `parseFtpTimestamp()`, `FtpEntry` |
| `FtpTransfer.h` | `planUpload()`, `planDownload()`, `runUpload()`, `runDownload()` — N parallel connections, retries, resume, partial clean-up; `commitDownloadedFile()` |
| `RemotePath.h` | remote path helpers (`normalizeRemotePath()` for typed input, `cleanRemotePath()`/`joinRemote()` that keep characters), untrusted-name checks `remoteNameProblem()`/`localNameProblem()`/`remoteChild()`, the protected-path guard, `classifyDestination()` |
| `PayloadSender.h` | `sendPayloadFile()`, `sendPayloadData()`, `probeTcpPort()` |
| `KlogClient.h` | `klogTag()`, `KlogLineSplitter`, `KlogClient` (background, reconnecting), `KlogSession` (`waitFor()`, `read()`) + `waitForKlogLine()` |
| `DpiInstaller.h` | `installPackage()` (HTTP multipart to `:12800/upload`), `dpiAccepted()` |
| `Y2jb.h` | regions, the param.json / appinfo.db / app.db patches and checks, `patchLocalFolder()`, `autoBlockOverFtp()`, `placeDownload0Dat()`, `validateY2jbIcon()`, `uploadY2jbIcon()`, `uploadY2jbArtSet()` |
| `ConsoleBackup.h` | `backupRemoteFile()`, `saveRemoteCopy()`, `newConsoleBackupDir()` |
| `MountConfig.h` | ShadowMount+/MicroMount `parse/render/validate`, `loadMountConfig()`, `pushMountConfig()`, `fetchMountDebugLog()`, `StopConfirmTracker`, `stopShadowMount()` |
| `Fakelib.h` | ShadowMount+ fakelib manager: `fakelib::list()`, `checkLocalFile()`, `add()`, `replace()`, `remove()`, `setEnabled()` |
| `RemoteTree.h` | `walkRemoteTree()`: recursive console listing (names only through `remoteChild()`, links not followed, MLSD times or MDTM, SIZE for missing sizes) |
| `ConsoleGames.h` | PS5 Manager: `listConsoleGames()` (+ `parseSiteAvail()`), `scanLocalImages()`, `mergeGames()`; folder compare `compareConsoleFolder()`, `compareFiles()`, `localFolderFiles()` |
| `Autoloader.h` | local `ps5_autoloader/autoload.txt`: `autoloader::parse/validate/normalize`, `defaultHeader()`, `insertLineAfter()`, `removePayloadLines()`, `load()`, `save()` (with `.bak`), `addPayload()`, `deletePayload()`, `payloadFiles()` |
| `Endpoint.h` | `ConsoleEndpoint`, `endpointFromSettings()` |
| `Discovery.h` | "Find my PS5": `subnetPrefixesFor()`, `localIpv4Addresses()`, `localSubnetPrefixes()`, `subnetHosts()`, `discoverConsoles()` |
| `FtpDiagnostics.h` | `diagnoseFtp()` (checks + speed test), `FtpDiagnosis` (JSON round trip), `classifyFtpServer()`, `kSpeedTestBytes` |
| `SocketWait.h` | internal: local-event-loop waits, connect with timeout, socket error messages |
| `Module.h` | `registerModule()`, setting keys, job kinds |

## Threading

All sockets are blocking-style: waits run a short local `QEventLoop` (25 ms tick for cancellation and timeouts) instead of `waitForReadyRead()`/`waitForBytesWritten()`, which Qt documents as unreliable on Windows. An `FtpClient`/`KlogSession` belongs to the thread that connects it. That thread **must be a Qt thread** (queue worker from `QThreadPool`, `QThread`, or the main thread): Qt's socket notifiers refuse `std::thread` (found by a test). `KlogClient` runs its own `QThread` and calls the line callback there. Parallel transfers start one `QThread` per connection, each with its own `FtpClient`.

## FTP

| Topic | Behaviour |
|---|---|
| Login | anonymous (`USER anonymous`, `PASS anonymous@`, like ftplib) or USER/PASS; 530 → "refused the login" |
| Greeting | waits for 220 (reads through 120); a server that never greets fails with a timeout |
| Data connection | passive by default: `PASV`, and `EPSV` once a server refuses `PASV`; connects to the control connection's host, not the PASV address (ftplib default). Active mode (`ps5.passiveMode` off): `PORT`/`EPRT` on the control socket's local address |
| Types | `TYPE I` for RETR/STOR/SIZE, `TYPE A` for listings, sent only when it changes |
| Listing | `CWD` into the folder, then `MLSD` when FEAT advertises MLSD/MLST (falls back to `LIST` on failure, like the legacy browser), else `LIST`. Parsers: Unix `ls -l` with owner+group (9 fields, the legacy format), Unix without a group column, DOS/IIS, MLSD facts; names keep inner spaces; symlinks reported, never followed |
| FEAT flags | the legacy `_ftp_probe_feat` rules (MLSD/MLST, REST STREAM, SIZE, MDTM, UTF8, XCRYPT, CPFR/CPTO/COPY) |
| Commands | CWD, PWD, MKD (+ `mkdirs` chain, existing segments ignored like `ensure_dir`), RMD, DELE, RNFR/RNTO, SIZE, MDTM, NLST, SITE AVAIL, NOOP, QUIT, recursive `removeTree` (children first, refuses `/`) |
| Transfers | `retrieve`/`store` stream through a `QIODevice` with 256 KiB chunks, ≤ 1 MiB queued, byte progress callback; `REST n` before RETR/STOR for resume, `APPE` when REST+STOR is refused |
| Timeouts | connect `ps5.connectTimeoutSeconds`; command replies max(connect, 5 s); a transfer that moves nothing for `ps5.idleTimeoutSeconds` fails ("stalled"); after a cancelled or timed-out transfer the client disconnects |
| Errors | user messages from reply codes (550 path rejected/not found, 552/452 out of space, 553 name not allowed, 5xx unsupported) and socket errors (refused → "no FTP server is listening on that port. Start it on the console…", the legacy `_friendly_ftp_error` advice). Every socket message names the console address and the service; the timeout text says the service "refused or ignored" the connection, because Windows retries a refused connect for about 2 s and a short timeout then reports no answer |
| Reply state | `lastReply()` describes the current transfer (reset at the start of `retrieve`/`store`, set from the reply to the RETR/STOR/APPE command itself and the final reply); `lastTransferStarted()` = the command was written (clean-up may be needed); `lastTransferAccepted()` = a 1xx arrived (the server opened the target) |
| Untrusted names | names from LIST/MLSD/NLST are never turned into paths blindly: `remoteChild()` refuses empty, `.`, `..` and names containing `/`, `\`, `:` or control characters; `removeTree()` refuses to recurse into such entries (and then does not remove the folder) |
| Safe replace | `uploadReplace()` = legacy `_ftp_upload_to`: STOR `<name><suffix>`, move the old file to `<name>.replacing.bak`, rename into place, delete the backup; on failure the temp is removed and the old file restored |

### Transfers (`FtpTransfer.h`, jobs `ps5.ftp.upload` / `ps5.ftp.download`)

* Layout as the legacy queue: a file → `<remoteDir>/<name>`, a folder → `<remoteDir>/<folder>/<relative path>`; empty folders are created too.
* All remote folders are created first on one connection; then `ps5.parallelTransfers` connections (job parameter `parallel` overrides) take files from a shared list. The legacy app uploaded strictly one file at a time.
* A lost connection, a timeout or a transient 4xx transfer reply (421/425/426/450/451) is retried `retries` times (default 2). Uploads resume from the server's `SIZE` **only when this run's STOR for that file was accepted** (1xx received); if the connection broke before that (e.g. during PASV), the remote file is still the older one and the retry rewrites from byte 0 (or from the offset `resume` approved), so an old file's head is never kept under new data. Downloads resume from the local `.part` size.
* `resume: true` also continues partial files left by an earlier run and skips remote files that already have the full size. It is off by default because a same-named older file on the console would be extended rather than replaced.
* **Cancel / final failure (uploads):** the half-written remote file is deleted over a fresh connection, like the legacy transfer queue, with the legacy log line "[FTP] Removed the partial remote file after cancel/failure: …"; the DELE is retried for about 2 s because the server may still hold the file of the aborted STOR (a Windows-hosted server cannot delete it until then); a file that could not be removed is reported. A STOR the server refused (5xx before any data) deletes nothing, so a file that was already on the console is never removed (the legacy cleanup would have deleted it).
* Downloads write `<name>.part`, flush it to the drive (`flushToDisk`) and rename it on success (`commitDownloadedFile()`: with `overwrite: true` the old file is set aside first and put back if the rename fails, and the complete `.part` is then kept); existing local files are skipped unless `overwrite: true`; free space is checked first; `.part` files are removed on cancel or a failed transfer. Console entries with unsafe names (`localNameProblem()`, which on Windows also covers `< > " | ? *`, trailing dots/spaces and device names) are skipped and listed in the job's warnings and `details.skippedNames`.
* The upload job reports its local sources as `busyPaths()`, so the queue never runs it beside a job that rewrites those files.
* Verification: every file's size is compared afterwards (remote `SIZE` / local size). The job fails (outcome kept) when a file failed; "Inconclusive" when the server cannot report sizes.
* Protected paths (legacy `_BR_PROTECTED`): `/system /system_ex /preinst /preinst2 /mini-syscore /sandcastle /update /dev /proc`, case-insensitive, after normalising `..` and doubled slashes. The legacy app asked for confirmation; the upload job refuses them unless `allowProtectedPaths: true` (the UI should show `protectedPathWarning()` and set it). Destination class (External USB / Internal / Unknown) is in the job details.

Upload parameters: `sources` (local files/folders), `remoteDir` (default `ps5.gamesFolder`), `host`, `port`, `parallel`, `resume`, `retries`, `allowProtectedPaths`. Download: `sources` (remote paths), `localDir`, `overwrite`, `resume`, `parallel`, `retries`, `host`, `port`.

## Find my PS5 (`Discovery.h`, legacy `_auto_detect_ip`)

* Subnets: the /24 around every IPv4 address of this PC's interfaces that are up and running (QNetworkInterface), skipping loopback interfaces and addresses, link-local 169.254/16, 0.0.0.0, multicast/reserved and `Virtual` (VPN tunnel) interfaces; duplicates removed. The legacy app used only the default-route interface (a UDP "connect" to 8.8.8.8). Hosts are `.1`–`.254` without the PC's own addresses.
* Probe (legacy values): ports 2121 then 2122, 300 ms connect timeout, 50 probes at a time. Every port of a host is tried (the legacy stopped at the first hit); `port` is the first open one, `openPorts` all of them, `rttMs` the first connect time. When a port accepts, the probe waits up to `bannerWaitMs` (400 ms) for an FTP greeting and keeps the `220` text as `banner`; nothing is sent (no login, no QUIT).
* Threads: `discoverConsoles()` blocks; each probe runs on a private `QThreadPool` (Qt-started threads, as Qt sockets require), the caller's thread waits in 25 ms steps, reports progress `(probed, total, found)` and, on cancel, drops the probes not started yet; running probes stop at their next 25 ms tick. Results are sorted by address.
* `DiscoveryOptions::hosts` replaces the sweep (tests use 127.0.0.1). No usable interface → `InvalidArgument` with advice.
* UI: PS5 connect › **Find my PS5** (`pages/ps5/FindConsole`): a sheet with the progress and the servers found; *Use this console* saves `ps5.consoleIp` and `ps5.ftpPort` (the legacy also filled both).

## FTP diagnostics and speed test (`FtpDiagnostics.h`, job `ps5.ftp.diagnose`)

Legacy `_ftpdiag_run` and `_ftpspeed_run` (LEGACY_UI_INVENTORY FTP-18). Parameters `{host?, port?, folder?, speedTest: bool}` (`folder` empty = `ps5.gamesFolder`; `speedTestBytes` only for tests). Checks, each recorded in `JobOutcome.details` (`FtpDiagnosis::toJson()`):

| Check | How | Legacy |
|---|---|---|
| Reachable, RTT | plain TCP connect to the FTP port, timed (`ps5.connectTimeoutSeconds`) | same (5 s) |
| Server banner, login | `FtpClient::connectToHost()` greeting; anonymous login | same |
| FEAT | `FtpClient::features()` (the legacy flags) and the raw reply | same |
| Passive mode | `PASV` (227), else `EPSV` (229) | same |
| Binary mode | `TYPE I` (2xx) | same |
| Writable | STOR 28 bytes as `<folder>/.exfatbuilder_wtest_<epoch>.tmp`, then DELE; a probe that cannot be removed is reported ("Wrote the probe but could NOT delete it…"); after a dropped STOR the probe is deleted over a fresh connection | same file and size |
| Destination | `classifyDestination(folder)` | `_ps5_dest_class` |
| Server type | `classifyFtpServer()`: XCRYPT +2, server-side copy +2, MLSD+UTF8+REST +1, banner "zftpd" +3 | same, **without the ZHTTP web-UI probe (+2), not ported** |
| Speed test (optional) | 16 MiB (1 MiB of random bytes repeated, like the legacy) STOR then RETR of `<folder>/.exfatbuilder_speedtest_<epoch>.bin`, MB/s = MiB per second each way; the read-back is compared byte for byte (new); then DELE, retried over fresh connections for about 2 s; a file that stays is reported. Cancel or a dropped transfer also deletes it (fresh connection, not cancellable) | same file, size and arithmetic; the legacy deleted only after a complete upload and never compared |

Outcome: **Failed** (the job fails, outcome kept) when the FTP server cannot be used (not reachable, no greeting, login refused) or the speed-test read-back differs; **Inconclusive** with warnings when passive or binary mode was refused, the folder is not writable, a probe or speed-test file was left behind, or the speed test did not finish; **Passed** otherwise. The summary reads e.g. "Reachable in 3 ms · logged in · /data/etaHEN/games writable · upload 38 MB/s, download 61 MB/s". Log lines start with `[FTP]`. Phases: Connecting · Checking · Speed test (bytes). UI: PS5 connect › **Diagnostics** (`pages/ps5/DiagnosticsTab`), and Home's setup checklist ("FTP reachable") reads the last result.

## Payloads (`ps5.payload.send`)

Raw TCP like `_pl_send`: connect (`ps5.connectTimeoutSeconds`, legacy 10 s), 64 KiB chunks, fail after `ps5.idleTimeoutSeconds` without progress (legacy 30 s), close; nothing read back. `target`: `payload` → `ps5.payloadPort` (9090), `shadowmount` → `ps5.shadowMountPort` (9021), `micromount` → `ps5.microMountPort` (9021); `port` overrides. The outcome carries the legacy warning about resident payloads.

## Kernel log

`KlogClient`: TCP to `ps5.klogPort` (3232), lines split on `\n` with `\r` stripped and empty lines dropped, UTF-8 decoded per complete line (the legacy decoded per 4 KiB read and could split characters), legacy keyword tags. Reconnects after a configurable delay (the legacy viewer did not reconnect). The line buffer for the viewer is the UI's job (legacy defect 4: unbounded).
`KlogSession::waitFor(matcher, T)` / `waitForKlogLine()`: Matched / Timeout / ConnectionLost / Cancelled, with every line seen.

## ShadowMount+ "Safe to unplug" (`ps5.shadowmount.stop`, legacy `_stop_shadowmount`)

1. Open a private klog session first (5 s); 2. create `/data/shadowmount` (best effort) and STOR an empty `/data/shadowmount/STOP`; 3. watch klog ≤ 45 s: confirmed at once by `shadowmountplus.elf calls exit`; otherwise, after `[shutdown] stop requested`, every `unmount start` seen after it must have reached `unmount complete` **and no new `unmount start` may appear for `StopConfirmTracker::kSettleMs` (6 s)** — ShadowMount+ may release images one after another, so matching counts alone are not proof (the legacy app confirmed on the first match). Lines are read in 200 ms slices (`KlogSession::read()`) so the quiet period can complete. Green only on confirmation; NoKlog / KlogLost / Timeout are amber (`Verification::Inconclusive` + warning).

## Config editors (`MountConfig.h`)

ShadowMount+ `/data/shadowmount/config.ini` and MicroMount `/data/micromount/config.ini`: defaults, `parseMountConfig()` and `renderMountConfig()` are the legacy `_parse_ini`/`_render_ini` (render output is compared in the tests with output **generated by running the legacy Python functions**): list keys (`kstuff_no_pause`, `kstuff_delay`, `scanpath`, `global_fakelib_exclude`; MicroMount `scanpath`, plus its `scan_paths` comma/semicolon variant) are one repeated key per line, `#` lines dropped; `image_ro/image_rw/image_sector` collected as `per_image_rules` (lines without `=` dropped); empty values omitted. `validateMountConfig()` = the legacy push checks (ShadowMount+: delays/intervals whole numbers 0–3600, `kstuff_delay` = `TITLEID:SECONDS`; MicroMount: `int(v, 0)` ≥ 0).
`pushMountConfig()`: local backup of the current remote config, best-effort `MKD`, STOR `config.ini.new` and swap it in (the ShadowMount+ legacy pattern, now also for MicroMount, whose legacy push was a direct STOR); a server that cannot rename gets the legacy direct-STOR fallback. `loadMountConfig()` reports "does not exist yet" as `NotFound` (normal on first use).

## Fakelib manager (`Fakelib.h`, legacy ShadowMount+ › Fakelib Manager, SMP-24)

| Topic | Behaviour |
|---|---|
| Folders | `/data/shadowmount/fakelib` (enabled) and `/data/shadowmount/fakelib/disabled`; a missing folder lists as empty (`Listing::folderExists` false for a fresh console) |
| Listing | `.prx`/`.sprx` files only (any case); folders, other files and the temporaries of a safe replace (`*.uploading`, `*.replacing.bak`) are skipped (legacy `FAKELIB_SKIP_SUFFIX`); names that fail `remoteNameProblem()` are skipped and reported (`Listing::skipped`); sorted by name, enabled first |
| Local file check (`checkLocalFile()`) | **Reject**: a folder, `eboot.bin`, anything with an `sce_sys` path segment, images (`.exfat .ffpkg .ffpfs .ffpfsc`), not `.prx/.sprx`, a name the console cannot take. **Confirm** (legacy warning cases): the first 4 bytes are not ELF `7F 45 4C 46` or SELF `4F 15 3D 1D` / `54 14 F5 EE`, or the file cannot be read. The legacy messages are kept. `add()`/`replace()` refuse a Confirm file unless `confirmed` is true |
| Add / Replace | `uploadReplace()` (legacy `_ftp_upload_to`): STOR `<name>.uploading`, old file to `<name>.replacing.bak`, rename into place, backup deleted; on failure the temporary is removed and the old overlay is left as it was. Add creates the folder (`mkdirs`) and uses the local file name; Replace keeps the overlay's name and folder. Uploads are held in memory (refused above 256 MiB) |
| Remove | `DELE` (the UI asks first; the legacy "cannot be undone" warning) |
| Enable / Disable | `RNFR`/`RNTO` between the folders, `disabled` created when needed. New: refused (`AlreadyExists`) when the other folder already holds a file of that name — the legacy rename would have replaced it |
| UI | PS5 connect › Fakelib (`pages/ps5/FakelibTab`): table (state badge, size, time, folder), Add…/Replace…/Enable-Disable/Remove/Refresh; each action opens its own connection and lists again (like the legacy worker threads) |

## Autoloader (`Autoloader.h`, legacy Y2JB › Autoloader, Y2J-16, `E:18825-19031`)

A **local** `ps5_autoloader` folder (setting `ps5.autoloaderFolder`, legacy `al_dir`); nothing is sent to the console. `autoload.txt`: one `.elf`/`.bin`/`.js` per line, `!<ms>` sleeps, `#` comments.

| Topic | Behaviour |
|---|---|
| Load | `autoload.txt` normalised like the legacy `_al_load` (CRLF/CR → LF, 3+ newlines → one blank line, exactly one trailing newline); a folder without it starts from the legacy header (`defaultHeader()`: search order, usage, "elfldr.elf (must be that filename!) line before other ELFs") — the first save creates the file |
| Validate (new) | errors: a line that is neither a comment, a sleep (`!` + digits) nor an `.elf/.bin/.js` name; warnings: `elfldr.elf` after another payload, a case variant of `elfldr.elf`, a payload not in the folder (or differing in case), a path instead of a name, `!0`, sleeps over 60 s, no payload at all |
| Save | normalised, UTF-8, LF, written atomically; the previous file is copied to `autoload.txt.bak` first (`.bak.2`, … — a backup is never overwritten); identical contents are not written again. The legacy save had no backup |
| Editing | chips for the folder's payloads (sorted case-insensitively): a click inserts the name on a new line below the cursor's line (`insertLineAfter()`, legacy `_al_insert_filename`), the bin icon deletes the file (to the recycle bin when the system has one, else deleted) and removes its lines plus the `!sleep` lines right after them from the editor (`removePayloadLines()`, legacy `_al_delete_payload`); "Insert !sleep…" (100–60000 ms, legacy dialog range); "Add payload…" copies a file in through a `.part` (replace only after confirmation) |
| UI | PS5 connect › Autoloader (`pages/ps5/AutoloaderTab`); the saved folder opens by itself (legacy) |

## Recursive console walk (`RemoteTree.h`)

`walkRemoteTree(client, root, {fillMissingSizes, exactTimes})`: depth first, per folder in case-insensitive name order, the files of a folder before its sub-folders' (legacy `collect_ftp_rows`). MLSD while the server takes it (times to the second), LIST once it answers MLSD with 5xx; `exactTimes` asks `MDTM` for every file the listing gave no exact time for (stops asking when MDTM is unsupported), `fillMissingSizes` asks `SIZE`. Names only through `remoteChild()` (others reported in `skippedNames`), links counted and never followed, an unreadable sub-folder skipped and reported (`unreadableDirs`); the root must be listable.

## PS5 Manager (`ConsoleGames.h`, legacy `E:13196-13670`, `ui/tab_ps5_mgr.py`)

| Topic | Behaviour |
|---|---|
| Game list | each configured console folder (UI field, `;`-separated, defaults to `ps5.gamesFolder`) listed with `LIST`/`MLSD`: images (`.exfat .ffpkg .ffpfs .ffpfsc`, legacy `_LIBRARY_IMAGE_EXTS`) with their sizes, plus sub-folders (game dumps, new); a folder that cannot be listed is reported, the others still listed |
| Free space | `SITE AVAIL` (raw reply shown); `parseSiteAvail()` takes the first number with an optional unit (B, KB/KiB … TB/TiB, 1024-based). The legacy took every digit of the whole reply as bytes |
| Local images | image files directly in one local folder (non-recursive like the legacy `img_scan_dirs` scan; UI default `storage.defaultOutputFolder`) |
| Merge | by file name (legacy): **On PS5** (same size or console size unknown), **Size mismatch**, **Local only**, **PS5 only**; folders are PS5 only. The legacy "total = free + listed sizes" estimate is not shown (it was not the drive size) |
| Folder compare (new) | `compareConsoleFolder()`: the console folder walked (`fillMissingSizes`) and matched by relative path, case-sensitive, against a local dump folder (`localFolderFiles()`) or the files of an `.exfat`/`.ffpkg` image (UI: `ImageTree`): **Missing on the PS5**, **Only on the PS5**, **Different size**; a size the console did not report is counted separately, not as a difference; unreadable folders, skipped names and links become warnings. Contents are not compared (that would download the game) |
| UI | PS5 connect › Manager (`pages/ps5/ManagerTab`): stats (games, local images, image bytes, free space), filter chips + search, Upload / Upload again (`ps5.ftp.upload` into the first folder or the mismatching copy's folder), Compare… (fills the compare folder from a game-folder row), the compare table with its own filter, Cancel. Firmware/etaHEN/temperature tiles are not offered (the legacy never filled them, defect 5) |

## AMPR index over FTP (`ampr.index.ftp`, integration module, inventory §3.7, ADV-21)

`engines/integration/AmprFtp.{h,cpp}` (it needs both this module's FTP client and the PFS engine's index writer):
1. one listing of the folder: it must contain `eboot.bin` or `sce_sys` (legacy `_probe_app0`), so a parent such as `/data/etaHEN/games` is never walked;
2. `walkRemoteTree()` with exact sizes and times (MLSD, else MDTM per file; a file with only a LIST time is counted as approximate and reported);
3. rows by the tool rules of the current drakmor ampr_emu upstream (`pfs::amprToolRows()`: the index, `.tmp`, `.bak`, `ampr_commands.bin` and `apr_emu.log` at the top are left out; paths with tab/CR/LF skipped; case-only variants keep the first in byte order) and the native `pfs::buildAmprIndex()` — **byte-identical to the index built from the same folder on disk** (`ampr.index.local`);
4. upload (upstream fix): STOR `ampr_emu.index.tmp`, existing `ampr_emu.index` → `ampr_emu.index.bak` (an older `.bak` replaced), rename the new one in, put the old one back if that fails; the legacy deleted the old index first;
5. RETR and byte compare (verification Passed/Failed).
Parameters `{remoteFolder, host?, port?}`; protected paths and `/` refused; cancel or a lost connection removes a leftover `.tmp` over a fresh connection. The local variant `ampr.index.local {folder, force?}` is in the PFS engine (`AmprJobs.h`): `<folder>/ampr_emu.index` through a `.part`, read back; without `force` a folder lacking `fakelib/libSceAmpr.sprx` is refused with the legacy "Not an APR title?" question (the UI asks it). UI: PS5 connect › AMPR index (`pages/ps5/AmprTab`), with a one-level-at-a-time console folder picker (legacy `_ampr_pick_ps5_folder`).

## DPI installer (`ps5.dpi.install`)

Exactly the legacy request: `POST /upload HTTP/1.1`, `Host`, `Accept-Encoding: identity` (what Python's http.client adds), `Content-Type: multipart/form-data; boundary=X-INSOMNIA-BOUNDARY`, manual `Content-Length`, one part `name="file"; filename="<pkg>"` (`"` in the name escaped as `%22`), `application/octet-stream`, 512 KiB blocks, closing boundary. Reply read by Content-Length, chunked encoding or connection close. Success: legacy rule "HTTP 200 or the body mentions success/install", except that a 4xx/5xx status or a body mentioning `fail`/`error` is never success (the legacy took "500 install failed" as success).

## Y2JB (`Y2jb.h`, job `ps5.y2jb.autoBlock`)

Regions USA `PPSA01650`, EU `PPSA01651`, JP `PPSA01652` (legacy single source of truth).

| Operation | Paths | Guard |
|---|---|---|
| Auto-block over FTP | `/user/appmeta/<TID>/param.json`, `/system_data/priv/appmeta/<TID>/param.json`, `/system_data/priv/mms/appinfo.db`, `/system_data/priv/mms/app.db` | RETR, size checked against `SIZE` (a short read is refused) → **local backup** (`<data>/ps5-backups/<time>/<remote path>`) → remote backup `param_backup.json` / `<db>.bak` (legacy, best effort, never over an existing one) → patch a temporary copy (databases: `PRAGMA integrity_check` must say `ok`) → 0 rows = not uploaded → upload to `<file>.uploading` and rename over the live file (`uploadReplace`; the live file is never truncated in place; a server that cannot rename gets "not changed" — no direct-STOR fallback) → RETR again (size-checked, integrity-checked) → verify. A failed upload or read-back puts the original back from the local backup and checks it ("restored from the local backup" / "still in place" / "could NOT be restored … saved at <path>"). Job requires `confirmed: true` (the legacy confirm text: be in the PS5 Settings menu). Fails (outcome kept) on any `✗` or `!`. |
| Patch local files | `<folder>/param.json`, `appinfo.db`, `app.db` | `.bak` next to each file before it changes (`.bak.2`, … if one exists — never overwritten), patch, verify |
| download0.dat | `MKD`, `/user/download/<TID>/download0.dat` | local backup of the existing remote file first |
| Icon | `/user/appmeta/<TID>/icon0.png` per region | PNG signature + IHDR exactly 512×512 (legacy check); local backup of the replaced file |
| Art set | `/user/appmeta/<TID>/` ← `pic0.png pic0.dds pic1.dds pic2.dds icon0.png`; `/user/catalog_downloader/appmeta/<TID>/` ← `icon0.png pic0.png` | overwrite only, never deletes, missing local files skipped; local backup of each replaced file |

Replacing by rename instead of the legacy in-place STOR was chosen so a dropped connection can never leave a truncated system database. **Unverified on a console:** the legacy app overwrote the file in place (same inode); after a rename, a system process that keeps the old database open could keep using it until the console restarts (a restart is already advised).

Patches: param.json `contentVersion = "99.999.999"`, `versionFileUri = "http://127.0.0.2"`, `localVersion = masterVersion` (each only when the key exists), edited **in place in the original text** (the legacy re-serialised the file with `json.dumps(indent=2)`); `tbl_appinfo` rows `VERSION_FILE_URI`/`CONTENT_VERSION` for the title in one transaction; `tbl_contentinfo.AppInfoJson` with the legacy SQL (`$.field_list[]` entries by key with `ELSE json(e.value)`, else top-level `json_set`), so the JSON is edited by SQLite and other entries/numbers are untouched. Read-back checks are the legacy `_y2jb_verify_*` rules. A backup that cannot be taken (remote file exists but is unreadable) refuses the write.

## Settings (section "PS5 connection")

| Key | Default | Read by | Legacy key |
|---|---|---|---|
| `ps5.consoleIp` | empty | every job (`host` parameter overrides) | `ftp_ip`; `ps5_ip` (never written by the legacy app; empty values are not imported) |
| `ps5.ftpPort` | 2121 | FTP jobs, Y2JB, Safe to unplug | `ftp_port` |
| `ps5.gamesFolder` | `/data/etaHEN/games/` | upload job default `remoteDir` | `ftp_path` |
| `ps5.payloadPort` | 9090 | payload job, target `payload` | `pl_port` |
| `ps5.shadowMountPort` | 9021 | payload job, target `shadowmount` | `smp_payload_port` |
| `ps5.microMountPort` | 9021 | payload job, target `micromount` | `mm_payload_port` |
| `ps5.klogPort` | 3232 | Safe to unplug (and `KlogClient` callers via `endpointFromSettings`) | `klog_port` |
| `ps5.dpiPort` | 12800 | DPI job | `y2jb_dpi_port` |
| `ps5.connectTimeoutSeconds` | 10 (1–120) | all connections | legacy fixed 10 s |
| `ps5.idleTimeoutSeconds` | 30 (5–600) | FTP transfers, payload sends, DPI | legacy fixed 30 s (payload send) |
| `ps5.parallelTransfers` | 2 (1–8, per job) | upload/download jobs | new (legacy serial) |
| `ps5.passiveMode` | on (advanced) | FTP data connections | legacy always passive |
| `ps5.autoloaderFolder` | empty | Autoloader tab (`autoloader::folderFromSettings()`; written when a folder is chosen) | `al_dir` |

Not mapped: `pl_ip` / `klog_ip` (one console address now), `y2jb_ftp_port` (Y2JB uses `ps5.ftpPort`), UI-only keys (`ftp_local_path`, `payloads`, `*_config` form dicts, `y2jb_pkg_*`, `y2jb_active_subtab`). The Manager tab remembers its folder fields in QSettings (`ps5Manager/*`, window state, not job settings).

Dependency probe `qt.sqlite` ("SQLite database support (Qt SQL)"): Ready when the `QSQLITE` driver loads and SQLite has JSON functions. On Windows the `sqldrivers/qsqlite.dll` plugin must be deployed next to the exe (`windeployqt` does this when Qt6Sql is linked).

## Job kinds

`ps5.ftp.upload`, `ps5.ftp.download`, `ps5.payload.send`, `ps5.dpi.install`, `ps5.y2jb.autoBlock`, `ps5.shadowmount.stop`, `ps5.ftp.diagnose`. Network discovery (`discoverConsoles()`), mount-config load/push, Y2JB local patching and file placement, the klog viewer and FTP browsing are APIs for the pages (run them off the UI thread).
`ps5.ftp.upload`, `ps5.ftp.download`, `ps5.payload.send`, `ps5.dpi.install`, `ps5.y2jb.autoBlock`, `ps5.shadowmount.stop`; elsewhere `ampr.index.ftp` (integration) and `ampr.index.local` (PFS). Mount-config load/push, Y2JB local patching and file placement, the klog viewer, FTP browsing, the fakelib manager, the autoloader editor, the game list and the folder compare are APIs for the pages (run them off the UI thread; the console-content tabs do it with `QtConcurrent` and a connection of their own per action, never through the FTP browser's `ConsoleSession`).

## What was tested, and against what

| Area | Tested against | Not tested |
|---|---|---|
| FTP login (anonymous, USER/PASS, refused), greeting timeout | `FakeFtpServer` | real ftpsrv / etaHEN FTP / zftpd |
| LIST Unix 9-field, Unix no-group, DOS, MLSD; parser edge cases (spaces, symlinks, years, cdir/pdir) | fake + unit tests | the exact LIST output of real PS5 servers (inventory U4) |
| PASV, EPSV fallback, active PORT | fake on 127.0.0.1 | EPRT/IPv6, NAT, Windows firewall prompts |
| Upload/download byte-exact (3 MiB), REST download offset | fake | multi-GB images |
| Parallel folder upload/download (25 files, ≥ 2 concurrent transfers observed) | fake (throttled) | console FTP servers' connection limits |
| Dropped data connection → retry with REST (both directions); `resume` of an earlier partial; already-complete skip | fake | a real Wi-Fi drop of the whole TCP connection |
| Control connection dropped on PASV before the STOR (reviewer reproducer): rewritten from 0, never appended to the old file; with `resume`, continues the approved partial only | fake | — |
| 425 / 451 replies to STOR/RETR retried; `lastReply()` reports them | fake | — |
| Cancel mid-upload → partial remote file deleted, also when the server refuses DELE while the aborted STOR still holds the file (Windows-like); refused STOR → existing file kept | fake | — |
| Unsafe names from LIST and MLSD (`..\..\x`, `a:ads`, `../x`, `x\y`) skipped by downloads and `removeTree`; `joinRemote` keeps characters | fake | real servers' handling of such names |
| Download commit: old file set aside and restored; `.part` kept when the folder refuses the rename (non-root only) | local | an antivirus lock on Windows |
| Stalled transfer → timeout | fake | — |
| Safe-swap replace, rename-refused fallback | fake | — |
| Payload byte-exact (5 MiB), refused, stalled receiver timeout, cancel | `FakeTcpSink` | the real ELF loader |
| klog reconnect, tags, split UTF-8, wait-for-line (match/timeout/lost) | `FakeLineServer` | klogsrv |
| Safe to unplug: two images released one after another (no confirmation at the first match), settle-time arithmetic, exit line, klog lost, no klog, timeout, no FTP | fakes + injected clock | ShadowMount+ itself |
| DPI request framing byte-exact, 200 / chunked / 500, refused | `FakeHttpUpload` | etaHEN DPI V2 |
| Y2JB param.json byte-preserving patch; appinfo.db/app.db patches incl. field_list schema, other entries kept, wrong region = 0 rows; local folder patch with backups; auto-block over FTP with local + remote backups and read-back, no temporary files left | SQLite fixtures built in the test + fake FTP | the console's real databases and schema |
| Y2JB: connection drop mid-upload leaves the live database untouched (no STOR over it); a silently truncated upload is detected and rolled back from the local backup; a short download is never used; `PRAGMA integrity_check` catches a damaged database | fake FTP with fault injection | rename semantics of the console's filesystem for open system databases |
| download0.dat / icon / art set placement with local backups | fake FTP | — |
| Mount config render = legacy output (generated from the legacy functions), parse, validation, load/push | golden text + fake FTP | — |
| Settings, legacy import, jobs end-to-end | in-process | the UI |
| Find my PS5: open ports and greeting on 127.0.0.1, a silent port, a closed port, an address nobody has (TEST-NET-1), progress counts, cancel during a 30 s greeting wait, subnet rules (loopback, link-local, multicast, duplicates) | `tst_ps5_diagnostics` (FakeFtpServer, local listeners); `tst_ps5_tools_ui` (sheet → settings, cancel) | a real /24 with a console and Windows' firewall prompt |
| Diagnostics: every check passing (MLSD+XCRYPT server), EPSV fallback, no passive mode, unreachable, refused login, unwritable folder, probe that cannot be deleted (left and reported), JSON round trip, the job's Passed/Failed/Inconclusive rules | `tst_ps5_diagnostics` (FakeFtpServer options `greeting`, `extraFeatures`, `refuseEpsv`, `refuseDeletePaths`) | ftpsrv / etaHEN FTP / zftpd replies |
| Speed test: 16 MiB both ways byte-exact, progress bytes, cleanup; silent corruption detected; undeletable file reported; cancel mid-upload (throttled server) deletes the file | `tst_ps5_diagnostics`; `tst_ps5_tools_ui` (Diagnostics tab end to end) | real Wi-Fi/Ethernet speeds |
| Fakelib manager: list (missing folder, skips, unsafe names), legacy reject/confirm rules (ELF and both SELF magics), add with folder creation, disable/enable, no overwrite of a same-named copy, safe replace without leftovers, remove; rename refused → original intact (`tst_ps5_content`); the tab end to end (`tst_app_ps5_tools`) | fake FTP (MLSD and LIST) | ShadowMount+ loading the overlays |
| Autoloader: normalisation, parse/validate, insert/remove helpers, header on first save, `.bak`/`.bak.2`, unchanged save, add/delete payloads, `al_dir` import (`tst_ps5_content`); the tab on a real folder (`tst_app_ps5_tools`) | local files | ps5_autoloader reading the file on a console |
| Recursive walk (order, sizes, MDTM times on a LIST server, unsafe names, links, root errors); game list over two folders + a missing one, SITE AVAIL, merge statuses; folder compare (missing/extra/size, identical, unknown sizes) (`tst_ps5_content`); Manager tab list, filters, compare with a dump and with an `.exfat` built by the engine (`tst_app_ps5_tools`) | fake FTP | real console listings, multi-thousand-file games over Wi-Fi |
| AMPR over FTP: index == local index byte for byte (MLSD and LIST+MDTM), `.bak` of the old index, second run stable, parent folder refused before any walk, protected/relative paths, rename refused → old index kept, cancel writes nothing; tool row rules; `ampr.index.local` incl. "Not an APR title?" (`tst_ampr_ftp`); the AMPR tab queueing both jobs (`tst_app_ps5_tools`) | fake FTP + real queue | the AMPR emulator reading an index built over FTP |

Builds and tests ran here on Linux (GCC 13 and Clang, Qt 6.4.2). The project's Windows CI (MSVC 2022, Qt 6.8.3) reported five connect/ps5 failures on the first version: two timing/message differences of a refused localhost connect, an RST that discards in-flight data in the fake, and a DELE refused while the aborted STOR still held the file; all are fixed (code where Windows behaves differently, test fakes where they assumed Linux), but the fixed version has not yet been re-run on that CI from here. Under root, the "remote file exists but is unreadable" backup refusal could not be exercised (the test skips it).

## Not implemented (legacy features outside this task or deferred)

The ZHTTP web-UI probe of the diagnostics (HTTP GET on the FTP port, "Open Web UI"); the PS5 Manager game compare (`SITE AVAIL` is available as `siteAvail()`); the ShadowMount+ fakelib manager; the AMPR index FTP walker; the autoloader editor; TLS (the legacy had none). The Discord webhook lives in `connect/notify` (`DiscordWebhook.h`).
FTP diagnostics (RTT, writable probe, ZHTTP/zftpd classification) and the 16 MiB speed test; the auto-detect /24 port sweep; the PS5 Manager's setup checklist, tool cards and activity list (the page header and the other tabs cover them) and its per-row cover art; per-game fakelib overlays (the legacy showed "Per-game — planned"); Discord/notification hooks; TLS (the legacy had none).
