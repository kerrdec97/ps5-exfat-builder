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
| `Endpoint.h` | `ConsoleEndpoint`, `endpointFromSettings()` |
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

## Payloads (`ps5.payload.send`)

Raw TCP like `_pl_send`: connect (`ps5.connectTimeoutSeconds`, legacy 10 s), 64 KiB chunks, fail after `ps5.idleTimeoutSeconds` without progress (legacy 30 s), close; nothing read back. `target`: `payload` → `ps5.payloadPort` (9090), `shadowmount` → `ps5.shadowMountPort` (9021), `micromount` → `ps5.microMountPort` (9021); `port` overrides. The outcome carries the legacy warning about resident payloads.

## Kernel log

`KlogClient`: TCP to `ps5.klogPort` (3232), lines split on `\n` with `\r` stripped and empty lines dropped, UTF-8 decoded per complete line (the legacy decoded per 4 KiB read and could split characters), legacy keyword tags. Reconnects after a configurable delay (the legacy viewer did not reconnect). The line buffer for the viewer is the UI's job (legacy defect 4: unbounded).
`KlogSession::waitFor(matcher, T)` / `waitForKlogLine()`: Matched / Timeout / ConnectionLost / Cancelled, with every line seen.

## ShadowMount+ "Safe to unplug" (`ps5.shadowmount.stop`, legacy `_stop_shadowmount`)

1. Open a private klog session first (5 s); 2. create `/data/shadowmount` (best effort) and STOR an empty `/data/shadowmount/STOP`; 3. watch klog ≤ 45 s: confirmed at once by `shadowmountplus.elf calls exit`; otherwise, after `[shutdown] stop requested`, every `unmount start` seen after it must have reached `unmount complete` **and no new `unmount start` may appear for `StopConfirmTracker::kSettleMs` (6 s)** — ShadowMount+ may release images one after another, so matching counts alone are not proof (the legacy app confirmed on the first match). Lines are read in 200 ms slices (`KlogSession::read()`) so the quiet period can complete. Green only on confirmation; NoKlog / KlogLost / Timeout are amber (`Verification::Inconclusive` + warning).

## Config editors (`MountConfig.h`)

ShadowMount+ `/data/shadowmount/config.ini` and MicroMount `/data/micromount/config.ini`: defaults, `parseMountConfig()` and `renderMountConfig()` are the legacy `_parse_ini`/`_render_ini` (render output is compared in the tests with output **generated by running the legacy Python functions**): list keys (`kstuff_no_pause`, `kstuff_delay`, `scanpath`, `global_fakelib_exclude`; MicroMount `scanpath`, plus its `scan_paths` comma/semicolon variant) are one repeated key per line, `#` lines dropped; `image_ro/image_rw/image_sector` collected as `per_image_rules` (lines without `=` dropped); empty values omitted. `validateMountConfig()` = the legacy push checks (ShadowMount+: delays/intervals whole numbers 0–3600, `kstuff_delay` = `TITLEID:SECONDS`; MicroMount: `int(v, 0)` ≥ 0).
`pushMountConfig()`: local backup of the current remote config, best-effort `MKD`, STOR `config.ini.new` and swap it in (the ShadowMount+ legacy pattern, now also for MicroMount, whose legacy push was a direct STOR); a server that cannot rename gets the legacy direct-STOR fallback. `loadMountConfig()` reports "does not exist yet" as `NotFound` (normal on first use). The fakelib manager is not implemented.

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

Not mapped: `pl_ip` / `klog_ip` (one console address now), `y2jb_ftp_port` (Y2JB uses `ps5.ftpPort`), UI-only keys (`ftp_local_path`, `payloads`, `al_dir`, `*_config` form dicts, `y2jb_pkg_*`, `y2jb_active_subtab`).

Dependency probe `qt.sqlite` ("SQLite database support (Qt SQL)"): Ready when the `QSQLITE` driver loads and SQLite has JSON functions. On Windows the `sqldrivers/qsqlite.dll` plugin must be deployed next to the exe (`windeployqt` does this when Qt6Sql is linked).

## Job kinds

`ps5.ftp.upload`, `ps5.ftp.download`, `ps5.payload.send`, `ps5.dpi.install`, `ps5.y2jb.autoBlock`, `ps5.shadowmount.stop`. Mount-config load/push, Y2JB local patching and file placement, the klog viewer and FTP browsing are APIs for the pages (run them off the UI thread).

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

Builds and tests ran here on Linux (GCC 13 and Clang, Qt 6.4.2). The project's Windows CI (MSVC 2022, Qt 6.8.3) reported five connect/ps5 failures on the first version: two timing/message differences of a refused localhost connect, an RST that discards in-flight data in the fake, and a DELE refused while the aborted STOR still held the file; all are fixed (code where Windows behaves differently, test fakes where they assumed Linux), but the fixed version has not yet been re-run on that CI from here. Under root, the "remote file exists but is unreadable" backup refusal could not be exercised (the test skips it).

## Not implemented (legacy features outside this task or deferred)

FTP diagnostics (RTT, writable probe, ZHTTP/zftpd classification) and the 16 MiB speed test; the auto-detect /24 port sweep; the PS5 Manager game compare (`SITE AVAIL` is available as `siteAvail()`); the ShadowMount+ fakelib manager; the AMPR index FTP walker; the autoloader editor; Discord/notification hooks; TLS (the legacy had none).
