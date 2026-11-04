# Legacy UI and PS5-connectivity inventory

Source: `PS5 exFAT Image Builder - Farewell Edition`, `APP_VERSION = '4.1.0'` (`exfat_builder.py:71`), Python/Tkinter, Windows only.
Checked-out branch at time of writing: `claude/wizardly-sagan-mahxhj` (based on `farewell-edition-dev`, HEAD `b298c2d`).
Method: static reading and grep/AST counting only. **The app was never executed** (Windows-only, Tk, admin elevation), so every runtime claim is "as coded". Anything not read directly or not provable from code is marked **UNVERIFIED**.

Scope of this file: shell/navigation, every page in `ui/*.py` outside the image-build/FPKG/Backport engines, PS5 connectivity, Library, i18n, updater.
Engine pages are listed as rows only and point to the sibling documents:
`LEGACY_INVENTORY.md` (image build, extract, convert, PFS, OSFMount), `FPKG_PARITY.md` (ffpkg editor/UFS2Tool), `BACKPORT_PARITY.md` (Backports).

Conventions: `file:line` are 1-based. `E:` = `exfat_builder.py`. Control IDs (`HOME-1`...) are used only so counts in section 9 are reproducible. "Persisted key" is a key in the settings JSON `~/.exfat_builder_settings.json` (`E:132-173`, atomic write via `.tmp` + `os.replace`, corrupt file renamed to `.corrupt`).

---

## 1. Navigation map

### 1.1 Shell (window chrome around the pages)

| Part | Behaviour | Source |
|---|---|---|
| Window | Title `exFAT Image Builder 4.1.0 — Farewell Edition  [Administrator]`; default 1280x820, min 960x640; geometry restored from `window_geometry` (clamped to screen); per-monitor DPI aware; app relaunches itself elevated (`ShellExecuteW runas`) | `E:4250-4303`, `E:4508-4523`, `E:37`, `E:916-930` |
| Application header (48 px) | Sidebar toggle (menu icon), product name, edition, version badge `v4.1.0`, current section label (`_header_section_var`), credits text `by DecKerr97 · Inspired by NookieAI • stonemodder (Porkfolio)`, icon buttons Help (-> Settings > Help) and Settings | `E:26770-26828` |
| Sidebar (232 px, 56 px compact) | Grouped nav, selected item gets 3 px accent pill; collapse state persisted as `sidebar_collapsed`; labels re-translated in place on language Apply; footer text `SYSTEM STATUS` + `exFAT Image Builder v4.1.0` (no live data) | `ui/shared/sidebar.py`, `E:4564-4658`, `E:22957` |
| Page area | One frame per registry key, `pack_forget` all then pack the active one; unknown key falls back to `unibuild` | `E:4994-5071` |
| PS5 status bar (28 px, bottom) | `PS5 ● Online/Offline (ip)` text, click = ping now; `FTP ● Idle` text; right-aligned build-queue text. Ping = TCP connect to ports 2121, 9021, 1234 (2 s each, first success = online) on IP `ps5_ip` falling back to `ftp_ip`; first ping 5 s after start, then every 30 s. `ps5_ip` is **never written anywhere** (only `ftp_ip`), so the fallback is what actually runs | `E:26837-26895` |
| Global OUTPUT LOG drawer | Collapsed by default, never auto-opens (`force_open` is a no-op); header buttons Copy, Clear; Text widget 9 lines | `E:26897-26990` |
| Keyboard | `Ctrl+B` build current queue; `Esc` cancel (not when focus is in Entry/Text/Combobox, only `unibuild` handled); `Ctrl+Q` quit; `F5` refresh; `Ctrl+Shift+T` test FTP. **`F5` and `Ctrl+Shift+T` are dead**: they call `_ps5_refresh`/`_imgs_refresh`/`_lib_refresh`/`_test_ftp_conn`, none of which is defined anywhere (grep) | `E:4317-4323`, `E:4094-4145` |
| Shortcut dialog | Modal listing 7 rows (the five above + "Double-click row = open build folder (History)" + "Drop folder = add to build queue"). Opened from Help page | `E:4147-4235` |
| Drag and drop | Optional `tkinterdnd2`; folder drop adds to build queue | `E:25-33`, `E:10716` |
| Crash handler | `sys.excepthook` writes `~/exfat_builder_logs/crash_<ts>.log` and offers to copy the report to the clipboard | tail of `E` |
| Close | Saves geometry/language/settings, optional close popup, stops tools | `E:3968-4030` |
| Startup jobs | Update check after 3 s (if `check_updates_on_boot`, default true); library auto-scan after 1.5 s (if `lib_auto_scan`, default true, and folders exist); OSFMount banner check after 0.5 s; release-notes modal after 0.9 s; temp-folder sweep in background | `E:4490-4500`, `E:4305-4313` |

### 1.2 Sidebar order (the only primary navigation; `E:4620-4638`, icons from `ui/shared/sidebar.py:36-41`)

| # | Sidebar group | Label (EN source string) | Registry key | Sidebar icon name | Page builder | Module |
|---|---|---|---|---|---|---|
| 1 | (none, pinned top) | Home | `home` | `home` | `_build_home_tab` | `ui/tab_home.py` |
| 2 | WORKFLOWS | Build | `unibuild` | `build` | `build_unibuild_tab` | `ui/tab_unibuild.py` (engine: LEGACY_INVENTORY.md) |
| 3 | WORKFLOWS | Extract | `extract` | `extract` | `_build_extract_tab` | `ui/tab_extract.py` (LEGACY_INVENTORY.md) |
| 4 | WORKFLOWS | Edit | `edit` | `edit` | `build_edit_tab` | `ui/tab_edit.py` (modes below) |
| 5 | MAIN TOOLS | Library | `library` | `library` | `_build_library_tab` | `ui/tab_library.py`, `ui/tab_images.py` |
| 6 | MAIN TOOLS | PS5 | `ps5` | `ps5` | `_build_ps5_tab` | `ui/tab_ps5_mgr.py` + 6 sub-modules |
| 7 | MAIN TOOLS | Convert | `convert` | `convert` | `_build_convert_tab` | `ui/tab_convert.py` (LEGACY_INVENTORY.md) |
| 8 | MAIN TOOLS | Dump Rename | `dumps` | `rename` | `_build_dumps_tab` | `ui/dump_rename.py` (`ui/tab_dump_rename.py` is a 13-line shim) |
| 9 | ADVANCED | Backports | `backports` | `backports` | `_build_backports_tab` | `ui/tab_backports.py` (BACKPORT_PARITY.md) |
| 10 | ADVANCED | Advanced | `advanced` | `advanced` | `_build_advanced_tab` | `ui/tab_advanced.py` |
| 11 | ADVANCED | History | `hist` | `history` | `_build_history_tab` | `ui/tab_history.py` |
| 12 | ADVANCED | Settings | `settings` | `settings` | `_build_settings_tab` | `ui/tab_settings.py` |
| 13 | ADVANCED | Credits | `about` | `credits` | `_build_about_tab` | `ui/tab_about.py` |
| 14 | ADVANCED | Report Issue | `report` | `bug` | `build_report_tab` | `ui/tab_report.py` |

Header-only entry points (no sidebar item): Help (`help` icon) and Settings icon buttons in the app header (`E:26810-26816`).
The registry (`E:4705-4770`) holds 25 keys; 14 are sidebar pages, 11 are legacy/hidden (see 1.4).
Default tab on first run is `home`; otherwise `last_tab` (persisted via a 1.5 s debounce, `_save_settings_debounced` `E:4938`). Startup remaps old keys: `build/ffpkg/pfs -> unibuild`, `files -> edit`, `images -> library>images`, `ftp/klog/payloads/y2jb -> ps5>sub`, `language/help -> settings>sub` (`E:4903-4932`).

### 1.3 Sub-tabs in order (via `build_subtabs`, `ui/shared/cards.py:565`; sub-panels are built lazily on first show; the selected sub-tab is **not** persisted unless a key is listed)

| Page | Sub-tab order (key / label / icon) | Persisted selection |
|---|---|---|
| Library | `dumps` Source Dumps (library) , `images` Built Images (drive) | none |
| PS5 | `mgr` Manager (ps5), `ftp` FTP (wifi), `klog` Kernel Log (klog), `payloads` Payloads (rocket), `y2jb` Y2JB (video), `smp` ShadowMount+ (zap), `micromount` MicroMount (folders) | none |
| Settings | `general` General (wrench), `language` Language (globe), `help` Help (help) | none (General rail pane: `settings_active_rail`, default `osfmount`) |
| Settings > General rail panes | `osfmount` OSFMount, `build` Build, `logs` Logs, `ftp` PS5 FTP, `notifications` Notifications, `theme` Theme, `shutdown` Auto-Shutdown | `settings_active_rail` |
| Advanced | `params` Build Parameters (wrench), `post` Post-Build (rocket), `lang` Language Stripper (globe), `ampr` AMPR Index (folders), `dlc` DLC-emu (database) | `adv_active_subtab`, default `params` |
| PS5 > FTP | segmented `upload` Send to PS5, `browser` PS5 Browser (default `browser`) | none |
| PS5 > Y2JB | `connection` (plug), `install` (package), `patch` (shield), `files` (files), `autoloader` (file_text) | `y2jb_active_subtab`, default `connection` |
| PS5 > ShadowMount+ | rail: Overview + 7 sections (see P09) | none |
| PS5 > MicroMount | rail: Overview + 4 sections (see P10) | none |
| Backports | `games` Games (ps5), `auto` Auto Backport (zap), `sdkfixer` SDK Fixer (wrench), `batchsdk` Batch SDK Retag (package), `results` Results (success); `advfixes` and `switcher` builders exist in the file but are not registered | none |
| Edit | modes `ffpkg` (default), `exfat`, `pfs`; "active format" pill | none |
| Build | single page with output-type selector exFAT / ffpkg / PFS | `output_dir` etc. |

### 1.4 Registry entries that are not sidebar pages (legacy / routed)

| Key | Label | Status |
|---|---|---|
| `build`, `ffpkg`, `pfs` | exFAT / ffpkg / PFS per-format Build pages | Still built (`'build (legacy)'`, `ffpkg`, `pfs` in `_tab_builders`, `E:4814-4850`) and still reachable by code that calls `_switch_tab('build')` (`E:12970`, `E:13178`, `E:22862`, `E:22887`, `E:23288`). `_LEGACY_SUBTAB_ROUTES` (`E:4976`) does **not** route them. Whether the user sees the legacy page or Build is **UNVERIFIED** (not executed). The rewrite must route every "send to Build" call site to the one Build page |
| `images` | My Images | routed to `library > images` |
| `files` | File Manager | routed to `edit > exfat` |
| `ftp`, `klog`, `payloads`, `y2jb` | PS5 sub-pages | routed to `ps5 > sub` |
| `language`, `help` | Settings sub-pages | routed to `settings > sub` |

---

## 2. Page specifications

Format per page: purpose; controls table (ID, label, type, default, persisted key); actions and backend; background work; outputs.
"-" in the key column = not persisted. Widget kinds map to `ui/shared/widgets.py` (section 8.2).

### P01 Home (`ui/tab_home.py`)

Purpose: landing page, goal-first launcher. No persisted settings, no backend logic; all cards call `_switch_tab`.

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| HOME-1 | Page title "exFAT Image Builder" / "What would you like to do?" | PageHeader | - | - |
| HOME-2 | Status card cell "PS5 console" (badge from `_ps5_status_var`, chevron -> `ps5`) | Badge + IconButton | "Not connected" | - |
| HOME-3 | Status card cell "FTP transfer" (badge from `_ftp_status_var`, chevron -> `ps5`; code routes to key `ps5`, not `ftp`, per tuple) | Badge + IconButton | "Idle" | - |
| HOME-4 | Status card cell "Build queue" (badge from `_queue_status_var`, chevron -> `unibuild`) | Badge + IconButton | "No active jobs" | - |
| HOME-5 | Action card "Build Image" -> `unibuild` (icon build) | Card button | - | - |
| HOME-6 | Action card "Extract Image" -> `extract` | Card button | - | - |
| HOME-7 | Action card "Convert Image" -> `convert` | Card button | - | - |
| HOME-8 | Action card "PS5 Tools" -> `ps5` | Card button | - | - |
| HOME-9 | Action card "Library" -> `library` | Card button | - | - |
| HOME-10 | Action card "Report Issue" -> `report` | Card button | - | - |

Layout: 3-column uniform grid, 16 px gaps, vertical scroll with auto-hiding scrollbar. Background work: none (badges follow StringVars via `trace_add`). Outputs: none.

### P02 Library > Source Dumps (`ui/tab_library.py`, engine `E:22015-22420`)

Purpose: scan folders of PS5 game dumps, show cover grid/list, send games to build/backport. Details in section 4.

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| LSD-1 | Add Folder (folder_plus) | Button | - | `lib_folders` (list of paths) |
| LSD-2 | Scan Folders (refresh) | Button | - | - |
| LSD-3 | Backport All (backports) -> `_lib_backport_all` (`E:22403`) | Button | - | - |
| LSD-4 | Build All (primary) -> `_lib_build_all` (`E:13144`) | Button | - | `output_dir` (asked if empty) |
| LSD-5 | Search "Search by title or title ID" (360 px) | TextField | empty | - |
| LSD-6 | View Grid / List | Segmented | `grid` | - |
| LSD-7 | Status text | Label | "No games scanned yet — add a folder and click Scan" | - |
| LSD-8 | Scan progress strip (spinner icon, "Scanning…", 6 px bar), shown only while scanning, hidden 1.5 s after done | ProgressBar | hidden | - |
| LSD-9 | Game tiles (grid: min 136, ideal 176, gap 16 px) / table rows (list) | Tile / row | - | - |
| LSD-10 | Tile context menu: Build, Backport, Open Folder, Details, Add to ffpkg Queue, Remove from library (title as disabled header) | Menu | - | - |
| LSD-11 | Rail card "Game details": cover, title, title ID, facts Version / Dump size / SDK (`N.xx`) / Modified / folder path | Card | "select a game" empty state | - |
| LSD-12 | Rail buttons Build, Backport | Button | - | - |
| LSD-13 | Rail action rows Open folder, Details, Add to ffpkg Queue, Remove from library | Link rows | - | - |
| LSD-14 | Rail card "Scanning folders": stats Games, Folders | Label | 0 / 0 | - |
| LSD-15 | Per-folder row: name, path, game count, Open folder icon, Stop scanning (x) icon (confirm dialog) | IconButtons | - | `lib_folders` |
| LSD-16 | Rescan button + "Last scan: dd/mm/YYYY HH:MM" | Button + Label | "Last scan: —" | - |
| LSD-17 | Details dialog: title, ID, facts, Open folder, Close | Dialog | - | - |

Actions: Add Folder `askdirectory('Select folder containing PS5 game dumps')`, normalised and de-duplicated (`E:21975`). Build / Build All append to the **legacy** exFAT queue (`self._queue`, `QueueItem`) or call `_uq_load_folder_and_add` when it exists, then `_switch_tab('build')` (`E:22853-22890`, `E:13144-13183`); Add to ffpkg Queue appends to `_ffpkg_queue` and switches to `ffpkg` (`E:22890-22913`). Remove from library only removes it from the in-memory list (`E:22914`, nothing on disk). Whether Build All lands in the unified Build page's queue is **UNVERIFIED**.
Background: scan runs on a daemon thread with generation counter `_lib_scan_gen` (stale scans abort); two passes (discovery, then size/cover/SDK enrichment with in-memory cache keyed by wrapper folder + signature of mtimes). Cache is not persisted.
Outputs: `[LIBRARY]` lines in the output log; games list in memory only.

### P03 Library > Built Images (`ui/tab_images.py`, engine `E:12856-13075`)

Purpose: list finished `.exfat/.ffpkg/.ffpfs/.ffpfsc` images in chosen folders and batch-upload to PS5.

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| IMG-1 | Add folder | Button | - | `img_scan_dirs` |
| IMG-2 | Re-scan | Button | - | - |
| IMG-3 | Send selected to PS5 (disabled until selection) | Button | - | - |
| IMG-4 | Search | TextField | empty | - |
| IMG-5 | Filter chip All / FW 4.51+ / Backport | Segmented | `all` | - |
| IMG-6 | Image tiles (cover, name, title ID, version, size, format badge, "Backport" badge, select check; click toggles selection) | Tile grid | - | - |
| IMG-7 | Rail "Selection" (count + total GB) and "Image folders" list with Open folder and remove per folder | Card | "No images selected" | `img_scan_dirs` |
| IMG-8 | Empty states "No Images Found" / "No matches" | EmptyState | - | - |

Rules: image found if file extension in `_LIBRARY_IMAGE_EXTS = ('.exfat','.ffpkg','.ffpfs','.ffpfsc')` (`E:12854`), non-recursive per folder. Backport badge/filter = file name contains `-bp` or `_bp` (case-insensitive). The "FW 4.51+" chip is a **no-op** ("FW is not known from the filename", `ui/tab_images.py:~280`). Title ID regex `(CUSA|PPSA|PPLH)[-_ ]?\d{5}`, version regex `v(\d+\.\d+)`.
Batch upload: confirm dialog (first 6 names), `_go_ftp_upload()` switches to PS5 > FTP > Send, then uploads one by one through `_ftp_upload_file(path, on_done=next)` (single-file direct STOR, **not** the transfer queue) (`E:13026-13066`).

### P04 PS5 > Manager (`ui/tab_ps5_mgr.py`)

Purpose: connection summary, setup checklist, shortcuts to PS5 tools, activity log and a game manager comparing local images with games on the console.

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| MGR-1 | Refresh All (primary) -> `_ps5mgr_refresh` | Button | - | - |
| MGR-2 | "PS5 Connection" card: status badge (`NOT TESTED` / `CONNECTED` / `OFFLINE`), Test Connection (same handler as MGR-1) | Badge + Button | NOT TESTED | - |
| MGR-3 | Facts: IP Address, FTP port, Kernel Log port, Payload port (read from settings: `ftp_ip`, `ftp_port` 2121, `klog_port` 3232, `pl_port` 9090) | Labels | - | read-only |
| MGR-4 | Hero stat grid (5 cols): Firmware, etaHEN, FTP, Payload, Temperature, Storage Used, Free Space, Games on PS5, Local Images. **Firmware, etaHEN, Temperature are never populated** (only `ftp`, payload, storage, counts are set) | StatGrid | "—" | - |
| MGR-5 | "Setup Checklist": 5 rows (PS5 IP configured, FTP reachable, Kernel Log configured, Payload configured, Payload folder configured), progress "n / 5 complete"; derived from settings `ftp_ip`, `klog_ip`, `pl_ip`, `al_dir` | Card | - | read-only |
| MGR-6 | "PS5 Tools" 6 cards with Open buttons: File Transfer (-> ftp), Send Payloads (-> payloads), Kernel Log (-> klog), Y2JB (-> y2jb), ShadowMount+ (-> smp), MicroMount (-> micromount); status badges for FTP (Online/Offline) and payload (Sent/Failed/Sending…) | Card x6 | - | - |
| MGR-7 | "Recent PS5 Activity" list (time, icon by kind, detail), max 20 | List | "No recent activity yet." | file `~/.exfat_builder_ps5_activity.json` |
| MGR-8 | "PS5 Game Manager": search, chip All / On PS5 / Local only / Size mismatch, legend (On PS5, Built locally not uploaded, Not built yet), table columns Game / Local / PS5 / Status / Path on PS5 with per-row actions (upload, re-upload, open folder) | Table | "Click Refresh All to load PS5 game list" | - |

Refresh algorithm (`E:13235-13335`, thread): local = files with image extensions in every dir of `img_scan_dirs`; remote = FTP connect (anonymous), `cwd(ftp_path)` (default `/data/etaHEN/games/`), `LIST`, parse `ls -l` style lines (>=9 tokens, name = tokens[8:], size = token[4]) keeping only image extensions; a second connection runs `SITE AVAIL` and sets free = digits of the response, total = free + sum of remote sizes (an estimate). Merge by file name -> `on_ps5` / `local_only` / `remote_only`; size mismatch badge when sizes differ. `ftp_ok` (connection success, not game count) drives CONNECTED/OFFLINE.
Row cover art: looked up in the Library list by title ID (`parse_meta_from_filename`, regex `(CUSA|PPSA|PPLH)[-_ ]?\d{5}`, `v\d+\.\d+`).
Activity log writes: payload sent/failed (`_ps5_activity_log`, `E:12065`); the 'connection' kind is declared but I found no writer (**UNVERIFIED**, grep of `_ps5_activity_log(` callers not exhaustive).

### P05 PS5 > FTP (`ui/tab_ftp.py`, backend `E:6140-7520`, `E:9877-10140`, `E:25630-26160`)

Purpose: file transfer to/from the console. Protocol details in section 3.2.

Header: title "File Transfer", segmented **Send to PS5 / PS5 Browser** (default Browser). Connection card above both views: `ip:port` text (`ftp_ip`/`ftp_port`, default port 2121), badge (Online / Offline / Not connected), status text `_br_status`, free-space text `_br_free`, buttons **Connect / Refresh** and **Disconnect**.

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| FTP-1 | Connect / Refresh -> `_br_connect` (anonymous FTP, then `_br_load(path)`) | Button | - | - |
| FTP-2 | Disconnect -> `_br_disconnect` (QUIT) | Button | - | - |
| FTP-3 | Send view: stat grid Connection, Target Path, Upload, Browser Path, Files Listed | StatGrid | OFFLINE / IDLE | - |
| FTP-4 | Upload type File / Folder | Radio | File | - |
| FTP-5 | Local file/folder path + Browse | TextField + Button | empty | - |
| FTP-6 | PS5 remote path + Default button | TextField + Button | `ftp_path` or `/data/etaHEN/games/` | `ftp_path` (only saved by Settings Save / `_save_ftp_settings`) |
| FTP-7 | Upload to PS5 (primary) -> `_ftptab_start_upload` | Button | - | - |
| FTP-8 | Cancel (shown while uploading) -> `_ftptab_cancel` | Button | hidden | - |
| FTP-9 | Progress bar, status line (`x.xx / y.yy GB • MB/s • ETA`), elapsed | Bar + Labels | - | - |
| FTP-10 | "Connection information" card: PS5 IP, FTP Port, Default Path (static `/data/etaHEN/games/`) | Labels | - | - |
| FTP-11 | Output log (inset) for the Quick upload | Console | - | - |
| FTP-12 | Browser view stat grid: Connection, Current Path, Files, Selected, Free, Upload Queue | StatGrid | - | - |
| FTP-13 | Local pane: path field, Home, up, refresh, new folder, rename, delete (confirm), listbox (double-click opens), status "N items • M selected • size" | Pane | `ftp_local_path` or home dir | `ftp_local_path` |
| FTP-14 | Transfer arrows: right (queue upload of local selection to current PS5 path, badge = count), left (download PS5 selection to local pane path); hint "Ctrl/Shift-select" | IconButtons | - | - |
| FTP-15 | PS5 pane: path field, Home (= `/data/etaHEN/games/`), up, refresh, new folder, rename, delete, quick-path dropdown (Root `/`, Data `/data`, User `/user`, USB0 `/mnt/usb0`, Games `/data/etaHEN/games/`), listbox (double-click folder opens; right-click menu) | Pane | `ftp_path` | `ftp_path` |
| FTP-16 | PS5 context menu: Open folder / Download, Rename, Cut, Move to…, Copy path, Delete; paste of a Cut buffer (`_br_paste`); upload here (File/Folder choice) | Menu | - | - |
| FTP-17 | "Transfers" panel: per-item row (name, Folder badge, size, `-> remote`, progress, speed, state badge, Cancel), stats text, Pause all (permanently disabled, no-op), Clear completed | List | "No transfers yet" | - |
| FTP-18 | "Connection Diagnostics": Run Diagnostics, Run Speed Test, Open Web UI; cells REACHABLE, RTT, SERVER, DEST, WRITABLE, FEAT; Advanced Details expander (PROTOCOL, PASSIVE, BINARY, SERVER-SIDE COPY, THROUGHPUT UPLOAD/DOWNLOAD) | Card | "Not run yet" | - |

Quick upload backend: `_ftptab_start_upload` (`E:9975`): thread; `_ftp_connect()`, `mkd` chain for the remote dir, `STOR` with blocksize 65536, folder mode walks the tree and recreates `remote/<folder>/<rel>`; cancel flag raises inside the progress callback; no partial-file cleanup on this path.
Single file shortcut: `_ftp_upload_file` (`E:26025`, used by Build "auto-upload", Images, History): same `STOR`, 64 KiB blocks, switches to the FTP > Send view, fires notification sound/toast, `_trigger_shutdown('ftp')` on success.
Queue backend: section 3.2.
Outputs: remote files; `[FTP]` log lines; Windows toast via PowerShell NotifyIcon (`E:26177`); optional Discord webhook (`discord_webhook` key, no UI to set it).

### P06 PS5 > Kernel Log (`ui/tab_klog.py`, backend `E:19033-19240`)

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| KLG-1 | PS5 IP | TextField | `klog_ip` or `ftp_ip` | `klog_ip` |
| KLG-2 | Port | TextField | 3232 | `klog_port` |
| KLG-3 | Connect (primary) / Disconnect / Pause (Pause enabled only while connected; toggles to Resume) | Buttons | - | - |
| KLG-4 | Status badge (Disconnected / Connecting… / Connected to ip:port / Paused — still receiving / Error: …) and caption "Default port: 3232 · TCP socket" | Badge | Disconnected | - |
| KLG-5 | 5 stat tiles: Total Lines, Lines/sec, Warnings, Errors, Connection (1 s poll of the line buffer) | StatTile x5 | 0 | - |
| KLG-6 | Toolbar: Clear, Export log, Auto-scroll, Word wrap, Timestamps, line counter "N lines" | Buttons + Switches | scroll on, wrap on, timestamps on | - |
| KLG-7 | Filter "Filter — type to highlight" + Clear (substring, case-insensitive; **filters** the view and highlights matches) | TextField | empty | - |
| KLG-8 | Inset console (Text), tags by keyword: error (`error fatal panic assert crash`), warning (`warn caution`), debug (`debug trace verbose`), else info | Console | - | - |

Behaviour: see 3.4. Export: `asksaveasfilename`, default `klog_YYYYmmdd_HHMMSS.txt`, header `PS5 Klog Export` + date + 60 `=`. Known quirks: the in-memory line list is unbounded; Resume does not replay lines received while paused (comment says "flush buffered lines" but no code does) — they appear only after a filter re-render; the Pause button label is overwritten with `⏸ Pause` text on disconnect (cosmetic).

### P07 PS5 > Payloads (`ui/tab_payloads.py`, backend `E:18478-18815`)

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| PLD-1 | Add payload -> dialog (Name, File with Browse `*.elf *.bin *.jar`, Notes), Save/Cancel, Enter/Esc | Button + Dialog | - | `payloads` (list of `{name,path,notes[,ip,port,last_sent]}`) |
| PLD-2 | Payload target: IP, Port, Save ("Default ports: 9090 / 9021") | TextField x2 + Button | `pl_ip` or `ftp_ip`; `pl_port` 9090 | `pl_ip`, `pl_port` |
| PLD-3 | Metrics: Payloads count, Console IP, Default Port, Last Sent (session time of last success) | StatGrid | - | - |
| PLD-4 | Search payloads | TextField | empty | - |
| PLD-5 | Payload rows (icon, name, notes, meta, per-row Send); click selects | List | - | - |
| PLD-6 | Detail card: name + READY/MISSING badge (file exists), file name + size, notes, File, Port, Per-payload IP, Last sent, TARGET, **Send to <ip>** (primary), Edit, Remove (confirm), warning bar "Sending a payload while another is resident can crash the kernel. Reboot between unrelated payloads." | Card | empty state "Select a payload to see details" | `payloads` |
| PLD-7 | Status line (`Sending <name> — n%  x MB/s`, `✓ Sent … in N s`, `✗ Send failed`) | Label | - | - |

Quirk: the detail card shows per-payload `ip`/`port` and a TARGET line using them, but `_pl_send` uses only the bar's IP/port (`E:18748-18760`), and the edit dialog has no IP/port fields, so those keys exist only in hand-edited JSON. Keep the record fields tolerated on import.

### P08 PS5 > Y2JB (`ui/tab_y2jb.py`, backend `E:17998-18480`, `E:20503-21740`)

"Y2JB" = YouTube to Jailbreak: installs the YouTube PKG via etaHEN's Direct Package Installer, patches/blocks YouTube updates, places `download0.dat`, edits `ps5_autoloader` files. Header InfoBar: "Requires etaHEN running with Direct Package Installer V2 enabled · FTP server running on PS5 · All three regions can be patched."

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| Y2J-1 | Deployment card stat grid: PS5 IP, FTP Port, DPI Port, Install state | StatGrid | - | - |
| Y2J-2 | Quick actions: Browse PS5 (-> FTP browser), Upload Package (-> FTP send), Open etaHEN folder (-> browser at `/data/etaHEN/`) | Buttons | - | - |
| Y2J-3 | Install queue card: current step, "Last installed: —", last 3 feed lines | Labels | "No install running" | - |
| Y2J-4 | Connection tab: PS5 IP address | TextField | `ftp_ip` | `ftp_ip` (shared with FTP) |
| Y2J-5 | FTP port (hint "ftpsrv: 2121 / etaHEN: 1337") | TextField | 2121 | `y2jb_ftp_port` |
| Y2J-6 | DPI port ("Direct Package Installer V2 port (default 12800)") | TextField | 12800 | `y2jb_dpi_port` |
| Y2J-7 | Save Connection Settings, Test FTP, Test DPI | Buttons | - | writes the three keys |
| Y2J-8 | Install tab: 3 region cards USA / EU / JP (PPSA01650 / 01651 / 01652, "Patchable" badge): PKG path (readonly) + Browse (`*.pkg`), status "✓ Ready — x.xx GB" / "✗ File not found", Install <region> | Card x3 | empty | `y2jb_pkg_usa`, `y2jb_pkg_eu`, `y2jb_pkg_jp` |
| Y2J-9 | Install progress card: step text, ETA text, bar | Bar | - | - |
| Y2J-10 | Patch tab: Region radio USA (PPSA01650) / EU (PPSA01651) / JP (PPSA01652) + warn InfoBar | Radio | USA | - |
| Y2J-11 | Patch local files: folder (needs `app.db`, `appinfo.db`, `param.json`), Browse, **Patch Local Files** | Field + Buttons | empty | - |
| Y2J-12 | **Auto-Block over FTP** (danger) with confirm; warning "Stay in the PS5 Settings menu…"; status text | Button | - | - |
| Y2J-13 | Files tab: download0.dat region radio (shows `/user/download/PPSA0165x`), file field + Browse (`*.dat`), **Place on PS5**, status | Radio + Field + Button | USA | - |
| Y2J-14 | YouTube app icon: region checks (default all on), icon0.png field + Browse (must be PNG 512x512, validated), **Upload Icon to PS5** | Check x3 + Button | all on | - |
| Y2J-15 | Art set: region checks (all on), art folder + Browse, **Upload Art Set to PS5** | Check x3 + Button | all on | - |
| Y2J-16 | Autoloader tab: ps5_autoloader folder + Browse + Load; `autoload.txt` editor (Text, 8 rows); ELF file chips (click inserts name at cursor, delete icon); Add ELF file; Insert !sleep; Save autoload.txt | Field + Text + Buttons | `al_dir` | `al_dir` |
| Y2J-17 | Output log (inset, 6 rows) + Clear | Console | - | - |
| Y2J-18 | Gating: all Install / Auto-Block / Place / Upload buttons disabled while PS5 IP is empty (tooltip explains) | Behaviour | - | - |

Behaviour in section 3.5.

### P09 PS5 > ShadowMount+ (`ui/tab_shadowmount.py`, 3549 lines)

Purpose: editor for `/data/shadowmount/config.ini` on the console plus deployment helpers. Layout: toolbar/hero, 220 px rail with search + Overview + 7 sections, scrolling content, sticky save bar (shown only when dirty), 38 px log strip. Shortcuts inside the tab: `Ctrl+S` save locally, `Ctrl+Shift+S` push to PS5 (only when dirty).

| ID | Control (key in config.ini) | Type | Default | Key |
|---|---|---|---|---|
| SMP-1 | Hero buttons: Send Payload (primary), Safe to unplug, Fetch log, Load from PS5; dirty pill; status text; stats Configuration / Sections / Rules / TCP Port / Status | Buttons | - | `smp_payload_port` |
| SMP-2 | Rail search over sections/keys | TextField | empty | - |
| SMP-3 | Save bar: diff summary, Save (local), Push to PS5, Discard, diff modal | Buttons | hidden | `shadowmount_config` (dict, whole form) |
| SMP-4 | Kstuff: `kstuff_game_auto_toggle` "Auto pause/resume kstuff" | toggle | 1 | in `shadowmount_config` |
| SMP-5 | `kstuff_crash_detection` "Crash detection + autotune" (writes `autotune.ini`) | toggle | 1 | idem |
| SMP-6 | `kstuff_pause_delay_image_seconds` | int seconds | 25 | idem |
| SMP-7 | `kstuff_pause_delay_direct_seconds` | int seconds | 15 | idem |
| SMP-8 | `kstuff_no_pause` "Skip these title IDs" (one `PPSA12345` per line, repeated key) | textarea | empty | idem |
| SMP-9 | `kstuff_delay` "Per-title pause delay overrides" (`TITLEID:SECONDS`, repeated key) | textarea | empty | idem |
| SMP-10 | `mount_read_only` | toggle | 1 | idem |
| SMP-11 | `force_mount` "Force-mount damaged filesystems" | toggle | 0 | idem |
| SMP-12 | `app_install_all` "Use sceAppInstUtilAppInstallAll" | toggle | 0 | idem |
| SMP-13 | `exfat_backend` | select `lvd`/`md` | lvd | idem |
| SMP-14 | `ufs_backend` | select `lvd`/`md` | lvd | idem |
| SMP-15 | `scan_depth` | select `1`/`2` | 1 | idem |
| SMP-16 | `scan_interval_seconds` | int seconds | 60 | idem |
| SMP-17 | `stability_wait_seconds` | int seconds | 10 | idem |
| SMP-18 | `scanpath` "Custom scan paths" (absolute, one per line, repeated key) | textarea | empty | idem |
| SMP-19 | `backport_fakelib` "Sandbox fakelib watcher" | toggle | 1 | idem |
| SMP-20 | `global_fakelib` "Global fakelib overlay" | toggle | 1 | idem |
| SMP-21 | `global_fakelib_path` | text | `/data/shadowmount/fakelib` | idem |
| SMP-22 | `global_fakelib_priority` | select `game`/`global` | game | idem |
| SMP-23 | `global_fakelib_exclude` (title IDs, repeated key) | textarea | empty | idem |
| SMP-24 | Fakelib Manager: list of `.prx/.sprx` in `/data/shadowmount/fakelib` (and `/disabled`), Refresh, Add, Replace, Remove, Enable/Disable (move between folders) | Table + Buttons | - | - |
| SMP-25 | `per_image_rules` "Override rules" (lines `image_ro=<file>`, `image_rw=<file>`, `image_sector=<file>:<bytes>`; last match wins; only lines containing `=` are emitted) | textarea | empty | idem |
| SMP-26 | `debug` "Debug logging" (writes `/data/shadowmount/debug.log`) | toggle | 1 | idem |
| SMP-27 | `quiet_mode` | toggle | 0 | idem |
| SMP-28 | Sector sizes (int bytes): `lvd_exfat_sector_size` 512, `lvd_ufs_sector_size` 4096, `lvd_pfs_sector_size` 32768, `md_exfat_sector_size` 512, `md_ufs_sector_size` 512 (5 rows) | int | as listed | idem |
| SMP-29 | Payload section: `shadowmountplus.elf` path + Browse (local file), Payload port, Send | text + int | path empty; port 9021 | `smp_payload_path`, `smp_payload_port` |

Dirty tracking per key with rail dots; "Reset defaults" restores `DEFAULTS` (`ui/tab_shadowmount.py:121-146`). INI render order and format: section 3.6.

### P10 PS5 > MicroMount (`ui/tab_micromount.py`, 2067 lines)

Same architecture as P09 (own copy of `SettingRow`), remote `/data/micromount/config.ini`, debug log `/data/micromount/debug.log`. No "Safe to unplug" and no fakelib manager. Rail: Overview, Core, LVD profile, PFS profile, Payload.

| ID | Control (key) | Type | Default | Key |
|---|---|---|---|---|
| MM-1 | Hero: Send Payload, Fetch log, Load from PS5; Save/Push/Discard bar | Buttons | - | `micromount_config` (whole form) |
| MM-2 | `target_directory` "Target directory" | text | `/data/homebrew` | idem |
| MM-3 | `scanpath` "Scan paths" (`.ffpfsc` scan dirs, repeated key) | textarea | empty | idem |
| MM-4 | `scan_depth` | select `0`/`1`/`2` | 1 | idem |
| MM-5 | `scan_interval_seconds` | int seconds | 30 | idem |
| MM-6 | `debug` "Debug notifications" | toggle | 1 | idem |
| MM-7 | `lvd_image_type` | int | 0 | idem |
| MM-8 | `lvd_sector_size` | int bytes | 65536 | idem |
| MM-9 | `lvd_secondary_unit` | int bytes | 65536 | idem |
| MM-10 | `lvd_raw_flags` | text | `0x9` | idem |
| MM-11 | `pfs_fstype` | select `pfs`/`exfat` | pfs | idem |
| MM-12 | `pfs_mkeymode` | select `AC`/`FAKE`/`NONE` | AC | idem |
| MM-13 | `pfs_budgetid` | select `system`/`user` | system | idem |
| MM-14 | `pfs_sigverify` "Signature verification" | toggle | 0 | idem |
| MM-15 | `pfs_playgo` "Playgo streaming" | toggle | 0 | idem |
| MM-16 | `pfs_disc` "Disc mode" | toggle | 0 | idem |
| MM-17 | `pfs_use_ekpfs` "Use EKPFS" | toggle | 1 | idem |
| MM-18 | `pfs_read_only` "Read-only mount" | toggle | 1 | idem |
| MM-19 | `pfs_force` "Force-mount" (danger: "skips mount checks — may corrupt data") | toggle | 0 | idem |
| MM-20 | Payload: `micromount.elf` path + Browse, Payload port (TCP) | text + int | empty; 9021 | `mm_payload_path`, `mm_payload_port` |

### P11 Advanced (`ui/tab_advanced.py`)

Header + 5 sub-tabs. Build-parameter semantics and the engine belong to LEGACY_INVENTORY.md; the control inventory is here. Settings are written by `_save_adv_params` (`E:12040-12060`).

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| ADV-1 | Build Parameters > Filesystem: Cluster size (Auto = 64K options) | select | `Auto` | `adv_cluster` |
| ADV-2 | Sector size | select | `512` | `adv_sector` |
| ADV-3 | Performance: Threads | int | `1` | `adv_threads` |
| ADV-4 | Robocopy file retry count (/R) | int | `3` | `adv_retries` |
| ADV-5 | Retry wait (/W, s) | int | `3` | `adv_retry_wait` |
| ADV-6 | Write speed benchmark (1 GiB sequential on temp drive) badge + Run | badge + button | `—` | `adv_write_benchmark` |
| ADV-7 | Image options: Skip post-build re-mount check | toggle | off | `adv_skip_verify` |
| ADV-8 | Exclude hidden files | toggle | off | `adv_excl_hidden` |
| ADV-9 | Image size override (GB) (blank = auto) — also on Post-Build | text | empty | `adv_img_size_override` |
| ADV-10 | ffpkg parameters: block size (-b) | select | `65536` | `adv_ffpkg_block` |
| ADV-11 | fragment (-f) | select | `65536` | `adv_ffpkg_frag` |
| ADV-12 | sector size (-S) | locked (read-only, "4096 produces broken images on Windows") | 512 | - |
| ADV-13 | Min Free % (-m) | int | `0` | `adv_ffpkg_minfree` |
| ADV-14 | Bytes / inode (-i) | select | `262144` | `adv_ffpkg_inode` |
| ADV-15 | Profile card, Recommendations card (detected hardware, `_detect_hw` thread), Help card, Reset all, Save parameters | Cards + Buttons | - | `lowercase_names`, `adv_robo_log`, `adv_robo_log_dir` (also saved here) |
| ADV-16 | Post-Build > Upload: Auto-upload .exfat to PS5 via FTP | toggle | off | `adv_auto_upload` |
| ADV-17 | Verification: Checksum verification (SHA-256) | toggle | off | `adv_checksum_verify` |
| ADV-18 | Danger Zone: Delete source folder after exFAT build | toggle | off | `adv_delete_source` |
| ADV-19 | Danger Zone: Delete source folder after ffpkg build | toggle | off | `adv_delete_source_ffpkg` |
| ADV-20 | Language Stripper (BETA): mode Single game folder / Scan all games in a folder; Games folder + Scan + Browse; quick-keep chips EN only, JA only, DE only, FR only, ES only, EN+JA, EU only; per-language Keep checkboxes; Keep all, Keep none, **Strip Selected Languages**; stats Selected Folder, Languages Found, Selected, Estimated Saving, Space To Save, Files To Remove, New Estimated Size | mixed | mode `single` | `adv_lang_mode`, `adv_lang_folder` |
| ADV-21 | AMPR Index Builder: Generate From Folder (local `app0` dir, requires `fakelib/libSceAmpr.sprx` else "Not an APR title?" prompt), Generate From Connected PS5 (FTP; folder picker `_ampr_pick_ps5_folder`, confirm, build + upload `ampr_emu.index`); stats Files Indexed, Index Size, Output; status "Idle." | Buttons + stats | - | - |
| ADV-22 | DLC-emu (PHASE 1): app0 folder, entries list (max `MAX_ENTRIES`, size cap 32 KiB), Add entry, load/save `dlc_emu.ini`, validation line (errors/warnings/valid) | Editor | - | - |

The advanced tab also builds a "pill" count badge per sub-tab (`_update_tab_count`). Language Stripper deletes language folders / audio packs from the game tree (destructive; copy into the Qt version needs a confirm gate).

### P12 History (`ui/tab_history.py`, backend `E:10345`, `E:12102`, `E:20399-20500`)

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| HST-1 | Refresh | Button | - | - |
| HST-2 | Export CSV (columns Time, Title, Title ID, Version, Type, Success, Output, Folder) | Button | - | - |
| HST-3 | Clear history (confirm; deletes the file) | Button | - | - |
| HST-4 | Stat cards: Total builds, Success rate, Failures, Total built (sum of existing output file sizes) | StatCard x4 | 0 | - |
| HST-5 | Search "Search title, ID or path" | TextField | empty | - |
| HST-6 | Filter All / Successful / Failed | Segmented | All | - |
| HST-7 | "Since <date>" caption and result count | Labels | - | - |
| HST-8 | Table columns Game (cover thumb, title, ID · version), Type (exFAT/ffpkg/PFS badge from extension), Output (elided left, tooltip), Size, Duration, When (humanised), Status (Successful/Failed badge) | Table | - | - |
| HST-9 | Row actions: Re-queue (-> `_add_to_queue_from_path`, only if folder exists), View log (newest `*.log` in the logs dir whose name contains the sanitised title; falls back to opening the folder), Open folder | IconButtons | - | - |

Storage: `~/.exfat_builder_history.json`, list newest first, **max 50 entries**, record `{title,title_id,version,folder,output,success,time:'YYYY-mm-dd HH:MM:SS'}` (`E:12102-12135`). Corrupt file moved to `.corrupt`. The Duration column always renders a literal `—` (`ui/tab_history.py:538-541`); no duration is stored.

### P13 Settings > General (`ui/tab_settings.py`)

Left rail (LeftRail) with 7 panes. Each pane = page header + cards of rows.

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| SET-1 | OSFMount: "Path to osfmount.com", Detect, Browse (`OSFMount.exe osfmount.com`), found-status line with file version and architecture | field + buttons | auto | `osfmount_path` |
| SET-2 | Build > Temp folder "Working directory" (readonly), Browse, Reset | field + buttons | system temp | `temp_dir` |
| SET-3 | Cleanup: Clear Temp Files (danger) | Button | - | - |
| SET-4 | Auto-retry on failure: Retries dropdown Off/1x/2x/3x/5x | dropdown | Off | `retry_count` |
| SET-5 | Updates: Check for updates on startup | switch | on | `check_updates_on_boot` |
| SET-6 | Updates: Check now | Button | - | - |
| SET-7 | PFS Convert: delete source image after convert | switch | off | `pfs_delete_source_after_convert` |
| SET-8 | PFS Convert: build exFAT/ffpkg then pipeline to PFS | switch | off | `pipeline_to_pfs` |
| SET-9 | Extraction (.exfat): multi-threaded robocopy | switch | off | `exfat_extract_robocopy_mt` |
| SET-10 | Extraction: threads | int field | 32 | `exfat_extract_mt_threads` |
| SET-11 | PFS convert backend radio: `lazy` Lazy_MkPFS by Nazky (default) / `mkpfs` MkPFS by PSBrew (Experimental) | radio | lazy | `pfs_backend` |
| SET-12 | Override Lazy_MkPFS folder (Advanced) + Browse | field | empty | `experimental_mkpfs_folder` |
| SET-13 | Logs: Logs folder, Open, Browse | field + buttons | `~/exfat_builder_logs` | `logs_dir` |
| SET-14 | Logs: Clear all logs (danger) | Button | - | - |
| SET-15 | PS5 FTP: Console IP, Auto-detect, Save | field + buttons | empty | `ftp_ip` |
| SET-16 | PS5 FTP: FTP port | field | 2121 | `ftp_port` |
| SET-17 | PS5 FTP: PS5 games path, Default | field + button | `/data/etaHEN/games/` | `ftp_path` |
| SET-18 | PS5 FTP: Auto-upload after build | switch | off | `ftp_auto` |
| SET-19 | Diagnostics: Test Connection (`_ftp_test`: connect + login, shows result), Ping PS5 (`_ftp_ping`: TCP connect 3 s to the FTP port), Browse PS5 (-> FTP browser), Games on PS5 (-> Manager), Cancel Upload | Buttons | - | - |
| SET-20 | Notifications: Play sound on completion (`winsound.MB_ICONASTERISK`) | switch | on | `notify_sound` |
| SET-21 | Theme: palette cards. Only `dark` (graphite, labelled "Dark Purple" in UI text) exists; light theme is explicitly "planned" | card select | dark | `theme` |
| SET-22 | Auto-Shutdown: Action dropdown None / Shut down / Restart / Sleep | dropdown | none | `shutdown_action` |
| SET-23 | Auto-Shutdown: delay (seconds, countdown dialog before action) | int | 60 | `shutdown_delay` |
| SET-24 | Auto-Shutdown triggers: unibuild, exfat, ffpkg, backport, ftp, pfs (6 switches) | switch x6 | off | `shutdown_on_unibuild`, `shutdown_on_exfat`, `shutdown_on_ffpkg`, `shutdown_on_backport`, `shutdown_on_ftp`, `shutdown_on_pfs` |

Auto-detect IP (`E:26235-26286`): local /24 from a UDP "connect" to 8.8.8.8:80 (no packet sent), then 50-thread TCP scan of `.1-.254` on ports 2121 then 2122 with 0.3 s timeout; first hit fills `ftp_ip` and `ftp_port` and saves. Shutdown actions run `shutdown`, restart, or `rundll32.exe powrprof.dll,SetSuspendState 0 1 0` (`E:20256-20335`).
`discord_webhook` and `lib_auto_scan`, `auto_ip` are read at start-up but **no Settings control writes them** (grep) — hidden legacy settings; decide whether to keep.

### P14 Settings > Language (`ui/tab_language.py`) — details in section 5

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| LNG-1 | Header "Interface language" with "N languages · M fully translated · community-maintained" (N = 18 list rows, M = rows with 100% coverage = 2: both English entries) | PageHeader | - | - |
| LNG-2 | Apply (primary) -> `_apply_lang` | Button | - | `language` |
| LNG-3 | Export .json -> writes the selected language dict (`indent=2`, `ensure_ascii=False`, sorted keys); for English exports a key==key template | Button | - | - |
| LNG-4 | Import .json -> **not wired** (shows a notice to send translations to @kerrdec97) | Button | - | - |
| LNG-5 | Search languages | TextField | empty | - |
| LNG-6 | Language list: 18 rows (code tile such as DE, English name, native name, coverage %) | List | active language | - |
| LNG-7 | Right pane: head card, stats (Coverage %, Strings translated n/N, Currently applied, Contributors "@kerrdec97"), Live preview (BUILD exFAT IMAGE, Drop a folder above, Browse, Build All), Strings table (key -> value, "n / N") | Cards | - | - |

Apply is refused with a dialog when a build is running (`_building`, `_unified_active`, `_ffpkg_building`). Apply = `set_language`, save, `_rebuild_all_tabs()` (destroys and rebuilds every built page frame from `_tab_builders`, retranslates sidebar in place), then `_switch_tab('language')`.

### P15 Settings > Help (`ui/tab_help.py`)

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| HLP-1 | Open in browser (writes bundled/fetched HTML to a temp file and opens it) | Button | - | - |
| HLP-2 | Check for updated tutorial | Button | - | - |
| HLP-3 | Keyboard shortcuts (dialog) | Button | - | - |
| HLP-4 | Status line ("Showing bundled tutorial v4.1.0", "Tutorial loaded from GitHub ✓", "Showing bundled tutorial (offline)") | Label | - | - |
| HLP-5 | Document viewer: `tkinterweb.HtmlFrame` if importable, else built-in FAQ article (TOC "ON THIS PAGE", sections, Q and A) | Viewer | - | - |

The bundled tutorial is base64 HTML in `E:79` (14.9 KB, 22 `<h2>`), and it is **stale** (header says v1.7.1; describes old tabs exFAT/ffpkg/My Images/Files). `tkinterweb` is not in `requirements-build.txt`, so the packaged exe most likely always shows the fallback article (**UNVERIFIED**).

### P16 Credits (`ui/tab_about.py`)

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| ABT-1 | Support the tool (Ko-fi `https://ko-fi.com/deckerr9746220`), Report Issue (-> `report`) | Buttons | - | - |
| ABT-2 | Farewell card: name, "Farewell Edition", version, final-release note; buttons GitHub repository (`https://github.com/kerrdec97/ps5-exfat-builder`), What's new (release modal, forced), Changelog (`CHANGELOG.md` via `resource_path`, falls back to releases page), Third-party notices (`THIRD_PARTY_NOTICES.md`) | Card | - | - |
| ABT-3 | Contributors card (6 rows with GitHub link icons): Nazky - PS5 Auto Backport pipeline; BestPig - backport research and BackPork payload; drakmor - ShadowMountPlus exFAT creation reference; SvenGDK - UFS2Tool ffpkg backend; ps5-payload-dev - klogsrv kernel log streamer; NookieAI and stonemodder - inspiration (Porkfolio) | List | - | - |
| ABT-4 | Acknowledgements card (`_ACKNOWLEDGEMENTS` list) | List | - | - |
| ABT-5 | Supporters card: Total supporters, Total downloads · GitHub (sum of `download_count` of all assets over `GET /repos/kerrdec97/ps5-exfat-builder/releases?per_page=100`, 8 s timeout, None on failure), thank-you message, name list + "Anonymous supporters" | Card | donors from `donors.json` next to the exe, else built-in JSON `E:100-107` (5 names) | - |
| ABT-6 | Licences card + View third-party notices | Card | - | - |

### P17 Report Issue (`ui/tab_report.py`)

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| RPT-1 | Generate diagnostic report (primary) -> `create_bug_report_file` (`E:3460`) writes `~/exfat_builder_logs/exfat_builder_report_<ts>.txt` (app version, Windows info, free disk, recent build summary, last error/traceback, redacted settings, full output log). No upload | Button | - | - |
| RPT-2 | Status well: badge Not generated / Saved / Failed, path | Label | Not generated | - |
| RPT-3 | Open report folder, Copy path (disabled until generated) | Buttons | disabled | - |
| RPT-4 | Recent log preview (last 200 lines of the OUTPUT LOG widget) | Console | - | - |
| RPT-5 | Destination cards: GitHub Issues (`https://github.com/kerrdec97/ps5-exfat-builder/issues`), Telegram Group (`https://t.me/ps5exfatbuilder`), Telegram Direct (`@deckerr97`, copy), Discord (`scottish_deckerr`, copy) | Card x4 | - | - |

### P18 Edit > exFAT mode ("File Manager", `ui/tab_files.py`, backend `E:8872-9340`)

Purpose: mount an `.exfat` image through OSFMount and edit it in place. Engine details (OSFMount driver, drive letter, robocopy) are in LEGACY_INVENTORY.md; controls here.

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| FM-1 | Image path, Browse, Mount, Remount, Dismount; hero (title, PPSA, drive usage, badge NO IMAGE) | field + buttons | - | - |
| FM-2 | Toolbar: Up, Refresh, Add files, Add folder, New folder, Apply backport, Replace, Delete; breadcrumb | IconButtons | - | - |
| FM-3 | Table Name / Type / Size / Modified; footer "N selected · size", folder summary | Treeview | - | - |
| FM-4 | Inspector: name, size, Path, Type, Modified, SHA-256 (shortened), first 64 bytes hex, Replace, Extract, Delete | Card | "No selection" | - |

Drive letter chosen from `Z:` downward over unused letters (`range(25,3,-1)`, `E:9170`), mounting needs `osfmount.com` (`_fm_find_osf`).

### P19 Dump Rename (`ui/dump_rename.py`, backend `E:22993-23330`)

| ID | Control | Type | Default | Key |
|---|---|---|---|---|
| DRN-1 | Source folder + Browse + Scan | field + buttons | `dumps_last_src` | `dumps_last_src` |
| DRN-2 | Naming pattern radio: `ppsa` "PPSA only" (`PPSA01234`), `ppsa_title` (`PPSA01234 Spider-Man`), `ppsa_title_v` (`PPSA01234 Spider-Man (1.05)`) | radio | `ppsa_title_v` (code fallback; the shown radio default is **UNVERIFIED**) | - (not persisted) |
| DRN-3 | Select all / Select none; row checkbox (default checked), editable proposed name, size, status badge Ready / Needs review | Table | checked | - |
| DRN-4 | Rename selected (footer, progress); optional destination `_dumps_dst_var` (empty = in place) | Button | - | - |
| DRN-5 | KPI strip / detail pane stubs kept for legacy callbacks (no-ops) | - | - | - |

Rename rules in section 4.4.

### P20 Rows for pages whose engines are covered elsewhere

| Page | Registry key / module | Sub-structure (control inventory not repeated) | Reference |
|---|---|---|---|
| Build (unified) | `unibuild` / `ui/tab_unibuild.py` (3402 lines) | Output type exFAT/ffpkg/PFS, source folder, drop zone, output dir, naming preset (`name_preset`, `name_custom_template`), image size mode auto/custom, PFS source mode folder/image, intermediate via exFAT/ffpkg, compression level, mixed queue, staged progress, post-build FTP upload via `adv_auto_upload`/`ftp_auto`, shutdown hook | LEGACY_INVENTORY.md; FPKG settings keys `fpkg_*` -> FPKG_PARITY.md |
| Legacy Build pages | `ui/tab_exfat.py`, `ui/tab_ffpkg.py`, `ui/tab_pfs.py` | Hidden per-format builders (see 1.4) | LEGACY_INVENTORY.md |
| Extract | `extract` / `ui/tab_extract.py` | Format-routed: `.exfat` (OSFMount copy), `.ffpkg` (UFS2Tool), `.ffpfs/.ffpfsc` (mkpfs unpack) | LEGACY_INVENTORY.md |
| Convert | `convert` / `ui/tab_convert.py` | Two cards: exFAT -> ffpkg, ffpkg -> exFAT | LEGACY_INVENTORY.md |
| Edit > ffpkg mode | `ui/tab_ffpkg_edit.py` (4008 lines) | UFS2Tool verbs ls/add/delete/replace/rename/stat/extract; mkdir via `.keep` | FPKG_PARITY.md |
| Edit > PFS mode | `ui/tab_edit.py` + `ui/tab_pfs.py` | Workspace flow (mkpfs unpack, edit, rebuild via Build) | LEGACY_INVENTORY.md |
| Backports (5 sub-tabs) | `backports` / `ui/tab_backports.py`, `ui/panel_bp_auto.py`, `ui/backport_profiles.py`, `core/backport_safety.py` | Games, Auto Backport, SDK Fixer, Batch SDK Retag, Results | BACKPORT_PARITY.md |
| Audio / DLC helpers | `ui/audio_*.py`, `ui/dlc_emu_ini.py`, `ui/mkpfs_runner.py`, `ui/experimental_mkpfs.py`, `ui/progress_state.py` | Engine helpers, no pages | respective sibling documents; `dlc_emu_ini` UI is in P11 |

---

## 3. PS5 connectivity in detail

### 3.1 Port and endpoint catalogue (all defaults as coded)

| Purpose | Protocol | Default port | Configurable | Key | Evidence |
|---|---|---|---|---|---|
| FTP (browser, uploads, diagnostics, AMPR, ShadowMount/MicroMount config, Y2JB file ops) | FTP, anonymous, passive (ftplib default) | 2121 | yes | `ftp_port` (Y2JB has its own `y2jb_ftp_port`, default "2121") | `E:25637-25646` |
| FTP alt probed by auto-detect | TCP probe | 2122 | no (scan list `[2121, 2122]`) | - | `E:26254` |
| Payload sender (Payloads page) | Raw TCP, one-way push | 9090 | yes | `pl_port` | `E:18478`, `E:18736` |
| ShadowMount+/MicroMount ELF send | Raw TCP | 9021 | yes | `smp_payload_port`, `mm_payload_port` | `ui/tab_shadowmount.py:3158`, `ui/tab_micromount.py` |
| Kernel log | TCP stream of text lines (klogsrv by ps5-payload-dev) | 3232 | yes | `klog_port` | `E:19044` |
| Etahen Direct Package Installer V2 (Y2JB PKG install) | HTTP/1.1 POST multipart | 12800 | yes | `y2jb_dpi_port` | `E:20655` |
| PS5 status ping | TCP connect probe | 2121, then 9021, then 1234 | no | - | `E:26878` |
| FTP server web UI probe (zftpd "ZHTTP") | HTTP GET `/` on the FTP port | = FTP port | no | - | `E:6238` |
| Y2JB hint text | etaHEN FTP is documented in the UI as port 1337 | 1337 | via `y2jb_ftp_port` | - | `E:18064` |

There are no hard-coded credentials; all FTP logins are anonymous (`ftp.login()` with no arguments), except AMPR index `parse_ftp_root` which accepts `ftp://user:pass@host:port/path` (`ui/build_ampr_index.py`, app builds the URL without credentials).

### 3.2 FTP browser, transfers, diagnostics

| Topic | Exact behaviour | Evidence |
|---|---|---|
| Client | Python `ftplib.FTP`; `connect(ip, port, timeout=10)`, `login()` anonymous. **Passive mode is the ftplib default** (`set_pasv(True)` is only called in the diagnostics probe and the art uploader). No TLS, no keep-alive timer; browser connection is re-validated with `NOOP` before use (`_br_ensure`) | `E:25637`, `E:7240` |
| Connections | One shared "browser" connection (`_br_ftp[0]`) for listing/rename/delete/download/mkdir, a separate short-lived connection per upload queue item and per feature (diagnostics, speed test, Y2JB, ShadowMount push/load, AMPR) | `E:7046`, `E:6873` |
| Listing | After `FEAT` (cached per connection): if MLSD advertised use `cwd(path)` + `mlsd()` parsing `type=` (`cdir`/`pdir` skipped, `dir` = folder) and `size=`; otherwise `LIST` (Unix `ls -l` parsing: >=9 tokens, folder if line starts with `d`). If `cwd` fails and path is not `/`, falls back to `/` and reports "<path> not found — showing / instead" | `E:6738`, `E:7264` |
| Free space | `SITE AVAIL` after each listing; text shown as `Free: <raw reply>`. Format of the reply depends on the server (**UNVERIFIED**) | `E:7280` |
| Capabilities probed | `FEAT` flags: MLSD/MLST, REST (`REST STREAM`), SIZE, MDTM, UTF8, XCRYPT, and server-side copy (`CPFR`/`CPTO`/`COPY`) | `E:6161-6188` |
| Parallel / multi-threaded transfers | **None.** One dispatcher thread drains a FIFO queue; items upload strictly one at a time; one connection per item | `E:6829-6872` |
| Resume | **None.** `REST` is probed for display only; STOR always restarts from byte 0; no retry loop in the FTP path (the "Retries" setting applies to image builds) | `E:6873-7008` |
| Upload (queue) | `ensure_dir` creates every path segment with `MKD` (errors ignored); file STOR with blocksize 65536 and a callback; folders: walk local tree, remote = `<remote_dir>/<folder name>/<relative path with />`; speed = bytes over the last 5 s window; UI refresh throttled to 4 Hz | `E:6890-6960` |
| Cancel | Per item flag checked every block; on cancel/failure the half-written remote file is deleted over a fresh connection (queue path only); log lines `[FTP] Removed the partial remote file …` | `E:6985-7005` |
| Safe-replace upload | `_ftp_upload_to` (used by Fakelib Manager): STOR to `<name>.uploading`, then move existing final aside to `<name>.replacing.bak`, rename temp to final, delete backup; temp removed on failure | `E:25917-25996` |
| Download | `RETR` to a local file per selected file (files only, no folder recursion), sequential in one thread, no progress UI beyond status text | `E:7212-7238` |
| Delete | Recursive remove implemented by `LIST` + `DELE`/`RMD` (children first); each target passes the protected-path guard, plus a confirm dialog listing up to 8 names | `E:7147-7197` |
| Rename/move/cut/paste | `RNFR/RNTO` via `ftp.rename` (cut-and-paste and "Move to…" are renames; no server-side copy used even if advertised) | `E:7317-7440` |
| Protected paths (warn then allow) | `/system`, `/system_ex`, `/preinst`, `/preinst2`, `/mini-syscore`, `/sandcastle`, `/update`, `/dev`, `/proc` (case-insensitive prefix match on `/`-normalised path). Applies to upload, delete, rename, paste, new folder | `E:7100-7132` |
| Destination class | Pure string heuristic: `/mnt/usb*`, `/mnt/ext*`, `/mnt/sandisk*`, `/mnt/extusb*` = "External USB / exFAT" (ok); `/data*`, `/mnt/sandbox*`, `/system*`, `/user*` = "Internal PS5 storage" (warn); else unknown | `E:6140-6159` |
| Diagnostics run | Thread: TCP reachability + RTT timing, `ftp.getwelcome()`, login, `FEAT`, passive test (`PASV`, fallback `EPSV`), `TYPE I`, writable test (STOR `<dir>/.exfatbuilder_wtest_<epoch>.tmp` with 28 bytes then `DELE`; warns loudly if cleanup fails), ZHTTP probe (HTTP GET `/` on the FTP port, 3 s, looks for "zftpd"/"zftpd-logo"/HTML) and server classification (zftpd score from XCRYPT +2, ZHTTP +2, server-side copy +2, MLSD+UTF8+REST +1, banner "zftpd" +3; >=2 likely, >=4 very likely) | `E:6190-6528` |
| Speed test | 16 MiB (`_SPEEDTEST_BYTES = 16777216`) random payload to `<browser dir>/.exfatbuilder_speedtest_<epoch>.bin`, STOR then RETR, MB/s each way, then `DELE` (reports leftover if delete fails) | `E:6528-6655` |
| Single-file helpers | `_ftp_makedirs`, `_ftp_exists` (SIZE then NLST fallback) | `E:25890-25915` |

Native module notes: Qt 6 has **no QFtp**; implement a small FTP client on `QTcpSocket` (control + passive data connection, `PASV` with `EPSV` fallback, `TYPE I`, `FEAT`, `MLSD`/`LIST` parsing, `STOR`, `RETR`, `MKD`, `RMD`, `DELE`, `RNFR`/`RNTO`, `SIZE`, `SITE AVAIL`, `NOOP`, `QUIT`). Keep the one-connection-per-job model; resume/parallelism would be new features, not parity.

### 3.3 Payload sender (Payloads, ShadowMount+, MicroMount)

| Item | Value |
|---|---|
| Transport | `socket.AF_INET, SOCK_STREAM`; connect timeout 10 s, then send timeout 30 s; file read in 64 KiB chunks with `sendall`; the socket is closed when the file ends (no application framing, no handshake, no response read) |
| What is sent | The raw bytes of a `.elf` / `.bin` (Payloads also accepts `.jar` in the file picker; autoloader accepts `.elf .bin .js`). ShadowMount/MicroMount send only their own ELF |
| Port | Payloads page: `pl_port` (9090 default); ShadowMount+/MicroMount: 9021 (the usual ELF loader port); validated 1-65535 in ShadowMount+ only |
| IP | Payloads: `pl_ip` (falls back to `ftp_ip`); ShadowMount+/MicroMount: `ftp_ip` (shared FTP IP), not a payload-specific IP |
| UI feedback | Progress text, success `✓ Sent "<name>" to ip:port in N s`, error dialog with a checklist (PS5 on and jailbroken, IP, port 9090/9021, ready to receive); success also logs to the PS5 activity file and triggers the notification |
| Safety text | Warning bar: sending a payload while another is resident can crash the kernel |
| Evidence | `E:18736-18815`, `ui/tab_shadowmount.py:3111-3220`, `ui/tab_micromount.py:1904-1990` |

### 3.4 Kernel log viewer

| Item | Value |
|---|---|
| Transport | TCP connect (10 s timeout), then blocking `recv(4096)` loop with no timeout until the peer closes or Disconnect closes the socket |
| Framing | UTF-8 (`errors='replace'`) text, split on `\n`, `\r` stripped, empty lines dropped; no protocol commands are sent |
| Tags | `error` if any of `error fatal panic assert crash`; `warning` if `warn caution`; `debug` if `debug trace verbose`; else `info` |
| Timestamps | Local `[HH:MM:SS]  ` prefix added client-side when the switch is on (the stored line includes it; export writes stored lines) |
| Filter | Case-insensitive substring; rendering re-runs the whole buffer (hides non-matches, highlights the term) |
| Shared use | ShadowMount+ "Safe to unplug" opens its **own** temporary klog socket (`klog_port`, 5 s connect) and watches it up to 45 s, leaving the Klog tab connection untouched |

### 3.5 Y2JB tab file paths and commands

| Action | Protocol and exact paths | Evidence |
|---|---|---|
| Test FTP | `ftplib.FTP.connect(ip, ftp_port, timeout=5)`, `login()`, `quit()` | `E:20585` |
| Test DPI | TCP connect to `dpi_port` (5 s); messages for timeout / refused | `E:20605` |
| Install PKG | `http.client.HTTPConnection(ip, dpi_port, timeout=30)`; `POST /upload`; header `Content-Type: multipart/form-data; boundary=X-INSOMNIA-BOUNDARY`; manual `Content-Length`; body = part `Content-Disposition: form-data; name="file"; filename="<pkg name>"`, `Content-Type: application/octet-stream`, file bytes streamed in 512 KiB blocks, closing boundary; progress maps upload to 0-90 %, 95 % = waiting for reply; success if HTTP 200 or the body contains "success" or "install" | `E:20655-20760` |
| Region title IDs | USA `PPSA01650` (UP4381), EU `PPSA01651` (EP4381), JP `PPSA01652` (JP4381); example PKG names `UP4381-PPSA01650_00-YOUTUBESIEA00000_USA.pkg` (and `_EU`, `_JP`) appear only in the superseded `_y2jb_build_install` builder | `E:20923`, `E:18101-18107` |
| Place download0.dat | `MKD /user/download/PPSA0165x` then `STOR /user/download/PPSA0165x/download0.dat` | `E:20775-20822` |
| Upload app icon | PNG must be exactly 512x512 (checked); `STOR /user/appmeta/<TitleID>/icon0.png` for each ticked region | `E:20831-20928` |
| Upload art set | Per region, local folder files to two places, skipping missing: `/user/appmeta/<TitleID>/` <- `pic0.png pic0.dds pic1.dds pic2.dds icon0.png`; `/user/catalog_downloader/appmeta/<TitleID>/` <- `icon0.png pic0.png`; overwrite only, never deletes; summary dialog Uploaded/Skipped/Failed | `E:20936-21048` |
| Patch local files | Backup `.bak` next to each file, then edit: `param.json` (`contentVersion = "99.999.999"`, `versionFileUri = "http://127.0.0.2"`, `localVersion = masterVersion` when both exist), `appinfo.db` table `tbl_appinfo` rows `VERSION_FILE_URI`/`CONTENT_VERSION` for the title ID, `app.db` table `tbl_contentinfo.AppInfoJson` (patches `$.field_list[]` entries by `key` when present, else top-level keys via `json_set`); each patch is verified by re-reading | `E:21050-21468` |
| Auto-Block over FTP | RETR -> patch in temp file -> STOR back -> RETR again to verify, for: `/user/appmeta/<TID>/param.json`, `/system_data/priv/appmeta/<TID>/param.json`, `/system_data/priv/mms/appinfo.db`, `/system_data/priv/mms/app.db`; remote backups `param_backup.json`/`<file>.bak` written first (best effort); per-file status ✓ verified / ✗ verification failed / ○ not found / ! error; confirm dialog says to be in the PS5 Settings menu | `E:21469-21740` |
| Autoloader (local) | `ps5_autoloader/autoload.txt`: one ELF/BIN/JS per line, `!1000` = sleep 1000 ms, `#` comments; first run creates a header with load-order comments (`elfldr.elf` must be first when used); saved with LF, 3+ blank lines collapsed. Edits a **local** folder only; nothing is pushed to the PS5 by this tab | `E:18825-19031` |

### 3.6 ShadowMount+ and MicroMount remote protocol

| Item | ShadowMount+ | MicroMount |
|---|---|---|
| Remote config | `/data/shadowmount/config.ini` | `/data/micromount/config.ini` |
| Remote log | `/data/shadowmount/debug.log` (Fetch log), `/data/shadowmount/autotune.ini` (declared) | `/data/micromount/debug.log` |
| Load from PS5 | FTP `RETR`, parse INI (repeated keys collapsed into newline-joined text) | same |
| Push to PS5 | Confirm dialog; `MKD /data/shadowmount`; STOR to `config.ini.new` then rename over `config.ini` (delete first when rename fails); falls back to direct STOR; temp removed | same pattern (**UNVERIFIED** in detail; structure mirrors) |
| Payload | Local `shadowmountplus.elf` path, TCP 9021 | Local `micromount.elf`, TCP 9021 |
| Stop / "Safe to unplug" | Opens klog socket first, then STOR empty `/data/shadowmount/STOP`; waits up to 45 s for klog markers: `[shutdown] stop requested` (ack), `unmount start`, `unmount complete` (counted only after the ack), definitive `shadowmountplus.elf calls exit`; green only on confirmation; amber warning on timeout / klog lost / klog unavailable | none |
| Fakelib manager | Directory `/data/shadowmount/fakelib` (+ `/disabled`); accepts only `.prx/.sprx`; rejects folders, `eboot.bin`, anything under `sce_sys`, image files (`.exfat .ffpkg .ffpfs .ffpfsc`); warns (confirm) when the first 4 bytes are not ELF `7F 45 4C 46` or SELF `4F 15 3D 1D` / `54 14 F5 EE`; skip suffixes `.uploading`, `.replacing.bak`; upload uses the safe-swap `_ftp_upload_to`; enable/disable = FTP rename between `fakelib` and `fakelib/disabled` | none |
| INI format | Header comments, then `key=value` lines grouped as kstuff / mount / scanning / fakelib / per-image / advanced; empty values omitted; repeated keys for list fields; comment lines (`#`) in textareas dropped; per-image lines without `=` dropped (`_render_ini` `ui/tab_shadowmount.py:2701`, `ui/tab_micromount.py:1651`) | core / LVD / PFS groups |
| Local persistence | Whole form dict under `shadowmount_config` / `micromount_config` | |

### 3.7 AMPR index over FTP (Advanced > AMPR)

`ui/build_ampr_index.py` builds `ampr_emu.index` (header `AMPRIDX3`, sorted fixed-size records, NUL-terminated path blob, open-addressed hash slots; paths lower-cased with `/`). Source = local folder or `ftp://host:port/app0`; for the PS5 path the walker `collect_ftp_rows` lists directories recursively over FTP (connect timeout 30 s), writes the index to the chosen output, then uploads `ampr_emu.index.tmp` and renames to `ampr_emu.index` after deleting any old one (`ui/build_ampr_index.py:405-445`).

### 3.8 Not found / absent

No SSH, no WebSocket, no mDNS, no UDP discovery (auto-detect is a TCP port sweep only), no PS5 "content manager" beyond the Manager page game list, no CPU/temperature/firmware readout (tiles exist but are never filled).

---

## 4. Library

### 4.1 Discovery (`E:22015-22225`)

| Step | Rule |
|---|---|
| Roots | `lib_folders` (list). For each root, every immediate sub-folder is a candidate "wrapper". Skipped names (case-insensitive): `$sysreset found.000 lost+found $windows.~bt $windows.~ws recovery recycler $recycle.bin system volume information` and anything starting with `.` |
| Game root | `eboot.bin` in the wrapper, else in a child, else in a grandchild (3 levels); no `eboot.bin` = not a game (the old "looks like PPSA" fallback was removed) |
| De-duplication | `_lib_dedupe`: group by normalised title ID (upper-case, no `-`); keep larger size, then shallower wrapper, then shallower eboot depth; games without ID are all kept; sorted by title (case-insensitive) |
| Title ID fallback | If metadata gives none: regex `((?:PPSA|CUSA|PPLH)\d{5})` on the eboot folder name then the wrapper name |
| Enrichment (2nd pass) | cover via `_load_cover_art`; folder size via `_get_folder_size(wrapper)`; SDK via `_read_sfo_info` (`SYSTEM_VER`) then `_read_sdk_from_folder` on wrapper and eboot root |
| Scan control | Generation counter cancels older scans; progress per top-level folder |

### 4.2 Metadata sources (`E:176-520`)

| Source | Rule |
|---|---|
| `param.sfo` | Found by `find_meta_file(folder, ['param.sfo'])`: root, `sce_sys`, then `os.walk` limited to depth <= 5, case-insensitive name. Parsed by `parse_sfo` (magic `0x46535000`, header `<IIIII`, entries `<HHIII`; format `0x0204` UTF-8 string, `0x0404` uint32, else raw bytes). Title = `TITLE` or `TITLE_00`; ID = `TITLE_ID` (ignored if int); version = `VERSION` or `APP_VER` |
| `pfs-version.dat` | If present and first line matches `[\d.]+`, **overrides** the SFO version (real patch version) |
| `param.json` | Only if SFO gave neither title nor ID. `titleId`; `version` or `masterVersion`; title from `localizedParameters[defaultLanguage]` -> `en-US` -> `en-GB` -> first language with `titleName`, else top-level `titleName`/`name` |
| `nptitle.dat` | Title only: first line, if still no title |
| Version normalisation | `XX.XXX.XXX`: 2 parts get `.000` appended; segments zero-padded to 2/3/3 |
| SDK / firmware | SFO `SYSTEM_VER`/`SDK_VER`/`SYSTEM_VERSION`: PS5 packs `0xMMmm0000`; major accepted when 1..30 (shift 24 then 16). `param.json` fields use hex-as-BCD quad-word (`0x1270...` = 12.70, `0x1000...` = 10); non-BCD hex falls back to the top byte. Shown as `N.xx` in the UI. Detailed SDK/backport rules -> BACKPORT_PARITY.md |

### 4.3 Cover art

| Item | Rule |
|---|---|
| Source | **Local files only. No web API, no download.** `_load_cover_art(folder)` (`E:10645`): `<folder>/sce_sys/icon0.png`, `<folder>/icon0.png`, then `os.walk` up to depth 4 for `icon0.png` skipping `decrypted` and `fakelib`. (Other code `_find_cover` `E:14059` also tries `pic0.png`, `icon0.dds`, parent folder, used by Backports) |
| Normalisation | `_normalize_cover` (`E:721`): accepted formats `PNG JPEG WEBP BMP DDS`, pixel cap 8192x8192, border trim (transparent then solid colour, bounded to 25 %), scale-to-fill to **260x390 (2:3)**, centre crop, saved as PNG |
| Cache | `~/exfat_builder_logs/covers_norm/<sha1(abs path|mtime|size|260x390)>.png`; display scaling LANCZOS to requested height; PhotoImage cache per `(path,height)` in memory; loading on a daemon thread |
| Placeholder | `game` icon in a 2:3 box |
| Images tab | Reuses the same loader keyed by title ID from the Library list |

### 4.4 Naming and rename rules

| Rule | Detail |
|---|---|
| `sanitize_filename` (`E:275`) | Normalise curly quotes to ASCII, dashes/minus to `-`, `…` to `...`, bullets to space, drop ®©™℠; drop control chars; remove `\ / : * ? " < > |` **and** cmd metacharacters `& ^ % ! \` $`; collapse whitespace; strip leading/trailing space and dot; cap 80 chars; empty -> `game` |
| `build_exfat_name` (`E:509`) | `"<TITLE_ID> <Title> (<version>).exfat"` — ID, title (if different from ID), version in parentheses, then sanitised; empty -> `game.exfat`. ffpkg queue items use the same string with the extension swapped. Case option applied at call site (`lowercase_names`) |
| Build naming presets | `name_preset`, `name_custom_template` (unified Build page; semantics -> LEGACY_INVENTORY.md) |
| Dump Rename (`E:23012-23330`) | Pattern keys `ppsa` / `ppsa_title` / `ppsa_title_v`; name = ID [+ title if different] [+ ` (version)` for `ppsa_title_v`]; optional upper-casing of the ID; sanitised when enabled (default on) then `rstrip(' .')`; folder rename with `os.rename` (in place) or `shutil.move` into a destination root (created if missing); target that already exists is skipped with "Skipped — target exists"; runs on a worker thread with per-row status. Game root for a dump via `_dumps_find_eboot_root` (self or single child with `eboot.bin`, or a PPSA-looking name) |
| Image file names | History/Images parse IDs with `(CUSA|PPSA|PPLH)[-_ ]?\d{5}` and `v(\d+\.\d+)`; `-bp`/`_bp` marks backport builds |

---

## 5. i18n and updater

### 5.1 Strings and languages

| Item | Value | Evidence |
|---|---|---|
| Mechanism | `_(text)` returns `text` for `en`, else `_TRANSLATIONS[lang].get(text, text)`. The **English text itself is the key**; there is no ID table | `E:2239-2243` |
| Storage | One Python dict literal in the source: `_TRANSLATIONS = {lang: {english: translated}}` (`E:977-2010`), plus `_NAV_TRANSLATIONS` (`E:2015-2224`) merged in with `setdefault` (community strings win). Inline in `exfat_builder.py`; **no external files**, no gettext/.po/.json loaded at runtime | `E:2225-2231` |
| Languages (table codes) | 16: `zh de fr es pt ja ko ru ar it nl pl tr th vi id`; with the English source = **17 languages** (asserted in `tests/test_translations.py`) | AST count |
| Picker list | 18 rows in `ui/tab_language.py:61-80`: the 16 + `en` "English (UK)" + `en_us` "English (US)" (no table for either; both show English) | |
| Strings per language (after nav merge) | zh 126, all other 15 languages 117 each; **126 unique keys, 1 881 entries total**. Before the nav merge: zh 115, others 106 | AST count |
| `_()` call sites | 2 066 calls, 1 559 unique literal keys, 9 non-literal. **Only 62 of those 1 559 literals exist in the table**; the rest (and every hard-coded string such as most of Y2JB, ShadowMount+, MicroMount, FTP dialogs, status lines) stay English in every language | AST count |
| Coverage figure in UI | `n / max(len(dict))`, i.e. against 126, not against the number of UI strings | `ui/tab_language.py:608` |
| Untranslated pages | `ui/tab_unibuild.py` has no `_()` calls; `ui/tab_micromount.py` and `ui/tab_shadowmount.py` use plain English literals | per-file count |
| Format subtleties | Python `%`-format placeholders inside keys (`'Saved %d strings to %s'`); keys sometimes include leading glyphs/emoji or trailing colons (`'Games folder:'` used with `.rstrip(':')`); sub-tab labels combine glyph and text (`'\U0001f528 Build'`) and are then icon-stripped | |
| Selection | `settings['language']` (default `en`) read at start (`E:4453`), `set_language(code)` sets the module global `_LANG[0]` (`E:2245`). Change = Language page Apply (restart not required: all built page frames are destroyed and rebuilt, sidebar relabelled in place). No OS-locale detection | `E:22928-22987` |
| Export / import | Export writes the selected dict as pretty JSON (`indent=2`, UTF-8, keys sorted); Import is a stub | `ui/tab_language.py:415-460` |
| Right-to-left | `ar` is present but there is no RTL layout handling (**UNVERIFIED**, none found) | |

Native suggestion: keep English-as-key semantics for first import (generate `.ts` from the 126 keys x 16 languages), then extend to full coverage via `tr()`.

### 5.2 Updater (`E:26287-26730`)

| Step | Behaviour |
|---|---|
| Trigger | 3 s after start when `check_updates_on_boot` (default true); Settings > Build > Updates "Check now"; version compare ignores `v/V`, takes numeric dotted prefix, pads to 3 parts, compares tuples |
| Check | `GET https://api.github.com/repos/kerrdec97/ps5-exfat-builder/releases/latest`, header `User-Agent: ps5-exfat-builder`, 5 s timeout, field `tag_name`, `html_url`; silent on any error. No "no update" message |
| Prompt | 3-way dialog "Update available — v<x> is available! Current version: v4.1.0": Download & install / Open releases page (`https://github.com/kerrdec97/ps5-exfat-builder/releases`) / Remind me later |
| Preconditions | Only when frozen (`sys.frozen`), and no build running (`_building`, `_unified_active`, `_ffpkg_building`) |
| Download | Re-fetches latest release JSON (10 s); picks the first asset ending `.exe` not containing `Lite` and the `.sha256` asset not containing `Lite`; no `.sha256` = **fail closed** ("This release has no .sha256 checksum"); streams to `<exe>.new` with 64 KiB reads, 30 s socket timeout |
| Verification | Size >= 5 MB; exact match with asset `size` when given; checksum file parsed as first token, must match `[0-9a-f]{64}`; SHA-256 of the download must equal it; any parse/compare failure = abort and delete temp (fail closed). No signature verification |
| Install | Writes `<exe>_update.bat` (OEM code page): wait for old PID to exit (up to 30 s), copy exe to `<exe>.bak`, `move` new over old with 8 retries x 2 s, 3 s pause, `start`, wait 6 s and `tasklist` check; roll back from `.bak` if not running; delete `.bak` after 30 s if alive; batch deletes itself. Launched with a cleaned environment (`updater_environment`, drops PyInstaller/Tcl variables) and the app closes through `_on_close` |
| Other network calls | Help tutorial: `GET https://raw.githubusercontent.com/kerrdec97/ps5-exfat-builder/main/tutorial.html` (8 s, must start with `<!DOCTYPE` or `<html`); Credits download count (releases API, 8 s); release-notes repo stats (`ui/release_notes.py:392`); Discord webhook POST (if `discord_webhook` set); `webbrowser` links (Ko-fi, GitHub, Telegram, dotnet, OSFMount, Dokany) |
| Telemetry | None found (no analytics endpoints) |

---

## 6. Release notes and support prompts (`ui/release_notes.py`)

| Item | Detail |
|---|---|
| Data | Single `RELEASE` dict (version 4.1.0, title, summary, 4 highlights, 6 improvements, `supporters: 6`, `show_on_first_launch`) |
| First-launch modal | Shown once per version; gate key `release_last_seen_version`; also reachable from Credits "What's new" with `force=True` |
| Support nudges | `note_successful_operation`: counts successes (`support_op_count`), shows a small prompt every 10 operations or every 30 days (`support_prompt_last_ts`) |
| Assets | `assets/whatsnew/{advanced,backports,settings}.png` (127 KB, 196 KB, 14 KB); mentioned in `CHANGELOG.md:1121` for an older release but **no code references them** (grep) |
| Dialog frame | `dialog_shell` / `dialog_present` / `dialog_heading` are the shared modal chrome used by several dialogs |

---

## 7. Theme, icons, assets

| Item | Detail |
|---|---|
| Theme | Single dark "graphite" design system in `tkinter_theme.py`: surfaces `bg_0 #0d0f12`, `bg_1 #131519`, `bg_2 #191c21`, `bg_3 #20242a`, `bg_4 #272b32`, `bg_5 #30353c`; text `fg_0 #f3f5f8` ... `fg_6`; accent `#2a6de8` (hi `#6ea8ff`, lo `#122340`); success `#34c48a`, warn `#e0a030`, danger `#e5484d` (+ `_hi`, `_bg`). Legacy `purple`/`teal` tokens alias to the accent. Windows 11 geometry: control 32 px, radius 4 (controls) / 8 (containers), header 48, sidebar 232/56, nav item 36, row 36 |
| Fonts | `Segoe UI Variable Text` or `Segoe UI`; semibold from the Semibold family; mono `Cascadia Mono` or `Consolas`. Roles: display 20 pt, title 15, stat 16, subtitle 11, body_strong/body 10, caption 9, overline 8, code 9, code_sm 8 |
| DPI | `px(n)` scales 96-dpi lengths by `SCALE`; process is per-monitor DPI aware |
| Light theme | Not implemented (`theme` key saved, Settings says planned; `_toggle_theme` mutates globals and tells the user it applies on next launch) |
| Icons | `ui/icons.py` + `ui/icons_data.py`: **177** line icons (Lucide, ISC) stored as masks and recoloured/composited at runtime; `EMOJI_ICON` maps legacy emoji to names; names: see section 8.3 |
| Application icon | `controller.ico` (root, also embedded base64 `_ICO_B64` for `iconbitmap`) |
| Other bundled data | Embedded base64: tutorial HTML (`E:79`), build scripts `_BAT_B64`/`_PS1_B64`/`_ICO_B64`, UFS2Tool, Backport sources (other documents) |

---

## 8. Supporting inventories

### 8.1 Settings keys touched by in-scope code (110 distinct literal keys found across `exfat_builder.py` and `ui/*`; full list by owner)

| Owner | Keys |
|---|---|
| Shell | `last_tab`, `window_geometry`, `sidebar_collapsed`, `theme`, `language`, `check_updates_on_boot`, `recent_folders`, `release_last_seen_version`, `support_op_count`, `support_prompt_last_ts` |
| PS5 / FTP | `ftp_ip`, `ftp_port`, `ftp_path`, `ftp_auto`, `ftp_local_path`, `ps5_ip` (read only), `auto_ip` (read only), `pl_ip`, `pl_port`, `payloads`, `klog_ip`, `klog_port`, `al_dir`, `smp_payload_path`, `smp_payload_port`, `shadowmount_config`, `mm_payload_path`, `mm_payload_port`, `micromount_config`, `y2jb_ftp_port`, `y2jb_dpi_port`, `y2jb_pkg_usa`, `y2jb_pkg_eu`, `y2jb_pkg_jp`, `y2jb_active_subtab`, `discord_webhook` (read only) |
| Library | `lib_folders`, `lib_auto_scan` (read only), `img_scan_dirs`, `dumps_last_src` |
| Settings | `osfmount_path`, `temp_dir`, `logs_dir`, `retry_count`, `notify_sound`, `pfs_delete_source_after_convert`, `pipeline_to_pfs`, `exfat_extract_robocopy_mt`, `exfat_extract_mt_threads`, `pfs_backend`, `experimental_mkpfs_folder`, `settings_active_rail`, `shutdown_action`, `shutdown_delay`, `shutdown_on_unibuild/exfat/ffpkg/backport/ftp/pfs` |
| Advanced | `adv_cluster`, `adv_sector`, `adv_threads`, `adv_retries`, `adv_retry_wait`, `adv_skip_verify`, `adv_excl_hidden`, `adv_img_size_override`, `adv_ffpkg_block`, `adv_ffpkg_frag`, `adv_ffpkg_minfree`, `adv_ffpkg_inode`, `adv_auto_upload`, `adv_checksum_verify`, `adv_delete_source`, `adv_delete_source_ffpkg`, `adv_robo_log`, `adv_robo_log_dir`, `adv_write_benchmark`, `adv_active_subtab`, `adv_lang_mode`, `adv_lang_folder`, `lowercase_names` |
| Other files | `~/.exfat_builder_settings.json`, `~/.exfat_builder_history.json` (max 50), `~/.exfat_builder_ps5_activity.json` (max 20), `~/exfat_builder_logs/` (logs, reports, crash logs, `covers_norm/`), `donors.json` next to the exe |
| Out of scope keys (Build/FPKG/Backport) | `output_dir`, `name_preset`, `name_custom_template`, `ffpkg_*`, `fpkg_*`, `pfs_*` (others), `abp_*`, `bp_*`, `bpr_log`, `retry`... (see sibling documents) |

Migration note: `_migrate_settings` renames `exfat_experimental_mkpfs -> experimental_mkpfs_enabled` and `experimental_mkpfs_dir -> experimental_mkpfs_folder` (`E:112-130`). A native app should import this JSON once and keep key names.

### 8.2 Shared widget set (`ui/shared/widgets.py` and friends) -> Qt equivalents

| Legacy | Qt suggestion |
|---|---|
| `Button` (primary/secondary/subtle/danger, sizes md/sm, icon left/right), `IconButton` | `QPushButton`/`QToolButton` with property-based QSS variants |
| `TextField`, `Dropdown`, `Spin`, `Check`, `Radio`, `Switch` | `QLineEdit`, `QComboBox`, `QSpinBox`, `QCheckBox`, `QRadioButton`, custom toggle |
| `Card`, `Rounded`, `Divider`, `InfoBar` (info/warn/danger), `Badge` (neutral/success/warn/danger/outline), `PageHeader`, `SectionTitle`, `StatGrid`/`StatTile`/`StatCard`, `EmptyState` | `QFrame` subclasses |
| `Tabs`, `Segmented`, `build_subtabs` | `QTabBar`/`QStackedWidget`, button group |
| `Sidebar`/`NavItem` (grouped, collapsible, tooltip when compact) | custom `QListView`/`QTreeView` or `QToolButton` column |
| `ProgressBar`, `RoundedBar`, `StagedProgressBar`, `Toasts` | `QProgressBar`, overlay widget |
| `Tooltip`, `AppDialog` (`app_info/warning/error/success/confirm/danger/choose3/question/choose`) | `QToolTip`, `QDialog`/`QMessageBox` |
| `ConsoleView`/Text with tags | `QPlainTextEdit` + `QSyntaxHighlighter` or `QTextCharFormat` |
| `Expander`, `CoverLabel`, `DropZone`, `LabeledField`, `Toolbar`, `ControlHero` | custom widgets |
| `attach_scroll` (mouse wheel speed) | native scrolling |

### 8.3 Icon names (177) used by the sidebar and pages

`advanced alert archive arrow_down arrow_left arrow_right arrow_up audio backports ban bell binary box boxes braces bug build cable calendar chart check checklist chevron_down chevron_left chevron_right chevron_up circle clipboard clock close cloud_upload code coffee compress convert copy cpu credits database disc dot download drive edit eject error external extract eye eye_off file file_down file_edit file_plus file_search file_text file_up files filter fingerprint flask folder folder_in folder_open folder_out folder_plus folder_tree folders from_line game github globe grid grip hash heart help history home image info key keyboard klog language layers library lightbulb link list loader lock login logout mail maximize megaphone menu message microscope minimize minus monitor more more_v mount move music network octagon package palette pause pause_circle pencil picture play play_circle plug plus power ps5 queue radio refresh rename repeat replace report rocket router ruler satellite save scissors search send server settings share shield shrink sidebar sidebar_close sidebar_open skip sleep sort sparkles star stop stop_circle success sync tag target terminal timer to_line trash tv undo unlock unmount unplug upload usb users verified video wand warning wifi window wrench youtube zap`

---

## 9. Migration table

| Feature | Legacy location | Notes | Suggested C++ module |
|---|---|---|---|
| App shell, nav, registry, last-tab, redirects | `E:4705-5071`, `ui/shared/sidebar.py` | 14 sidebar pages + legacy redirects; lazy sub-panels; keep route table so "send to Build/PS5" call sites hit one page | `app/MainWindow`, `app/NavModel`, `app/PageRegistry` |
| Header, status bar, output-log drawer | `E:26770-26990` | Ping loop 30 s ports 2121/9021/1234; log drawer collapsed by default | `app/HeaderBar`, `app/StatusBar`, `app/LogDrawer` (Qt Network `QTcpSocket`) |
| Settings persistence | `E:132-173` | JSON in home dir, atomic replace, corrupt quarantine, migration of two keys; 110+ keys | `core/SettingsStore` (QJsonDocument, same path; one-time import) |
| History store | `E:10345`, `E:12102`, `ui/tab_history.py` | 50 entries, CSV export, re-queue, view log | `core/HistoryStore`, `ui/HistoryPage` |
| PS5 activity log | `E:12061-12090` | 20 entries JSON | `core/ActivityLog` |
| FTP client | `E:25637`, `E:6161-6240`, `E:6738` | Anonymous, passive, FEAT, MLSD/LIST, SITE AVAIL; Qt has no QFtp | `net/FtpClient` on `QTcpSocket` (or libcurl behind the same interface) |
| Transfer queue | `ui/tq_model.py`, `E:6759-7040` | Serial queue, per-item cancel, partial-file cleanup, folder = one item | `net/TransferQueue` (QObject, worker `QThread`) |
| Safe-replace upload | `E:25917` | `.uploading` then swap with `.replacing.bak` | `net/FtpClient::uploadReplace` |
| FTP browser UI | `ui/tab_ftp.py`, `E:7046-7520` | Dual pane, context menu, protected-path guard, cut/paste = rename | `ui/FtpBrowserPage`, `net/Ps5PathGuard` |
| FTP diagnostics and speed test | `E:6190-6655` | FEAT, PASV/EPSV, TYPE I, write test, ZHTTP probe, zftpd classifier, 16 MiB speed test | `net/FtpDiagnostics` (+ `QNetworkAccessManager` for the HTTP probe) |
| Quick upload ("Send to PS5") | `E:9975-10140`, `E:26025` | Single direct STOR, no resume, no temp name | merge into `net/TransferQueue` with option "direct" |
| Payload sender | `E:18736-18815` | Raw TCP, 10 s connect, 64 KiB chunks, no handshake | `net/PayloadSender` (`QTcpSocket`) |
| Payload list and dialogs | `ui/tab_payloads.py`, `E:18478-18735` | Records `{name,path,notes}`; per-payload ip/port ignored | `ui/PayloadsPage`, `core/PayloadStore` |
| Kernel log client and viewer | `E:19033-19240`, `ui/tab_klog.py` | Line-oriented TCP, keyword tags, export; add bounded buffer | `net/KlogClient`, `ui/KlogPage` |
| PS5 Manager and game compare | `E:13196-13670`, `ui/tab_ps5_mgr.py` | LIST parse, SITE AVAIL estimate; Firmware/etaHEN/Temperature tiles are empty | `ui/Ps5ManagerPage`, `core/GameCompare` |
| Auto-detect IP | `E:26235-26286` | /24 sweep ports 2121, 2122, 0.3 s, 50 workers | `net/Ps5Discovery` (`QtConcurrent` + `QTcpSocket`) |
| Y2JB: DPI test, PKG install | `E:20605-20760` | HTTP POST multipart to `:12800/upload`, boundary `X-INSOMNIA-BOUNDARY`, 512 KiB blocks, success rule 200 or text | `net/DpiInstaller` (`QNetworkAccessManager`, streaming `QIODevice`) |
| Y2JB file ops over FTP | `E:20775-21048` | download0.dat, icon0.png 512x512 check, art-set plan | `ps5/Y2jbService` using `FtpClient` |
| Y2JB patching (param.json, appinfo.db, app.db) | `E:21050-21740` | SQLite JSON schema detection (`field_list`), verify by read-back; backups | `ps5/Y2jbPatcher` (Qt SQL with SQLite JSON1 or sqlite3 directly) |
| Autoloader editor | `E:18825-19031` | Local `autoload.txt`, LF endings | `ps5/AutoloaderEditor` |
| ShadowMount+ config and stop flow | `ui/tab_shadowmount.py` | INI schema, dirty tracking, fakelib manager, STOP handshake via temporary klog | `ps5/ShadowMountService`, `ui/ShadowMountPage`, `core/IniSchema` |
| MicroMount config | `ui/tab_micromount.py` | Same shell, different schema | `ps5/MicroMountService`, `ui/MicroMountPage` |
| AMPR index builder | `ui/build_ampr_index.py`, `ui/tab_advanced.py:631-994` | Binary index writer + FTP walker | `tools/AmprIndex` (pure C++), reuse `FtpClient` |
| DLC-emu editor | `ui/dlc_emu_ini.py`, `ui/tab_advanced.py:994-1749` | Max entries, 32 KiB cap | `tools/DlcEmuIni` |
| Language stripper | `ui/tab_advanced.py:1769-2010`, `ui/audio_*` | Destructive; needs confirm | `tools/LanguageStripper` |
| Library scan | `E:22015-22340` | Wrapper/eboot depth 3, dedupe by title ID, async enrichment, in-memory cache | `library/GameScanner` (`QtConcurrent`), `library/LibraryModel` |
| SFO / param.json / nptitle / pfs-version parsing | `E:176-500` | Exact fallback order and version padding | `library/Sfo`, `library/ParamJson`, `library/GameInfo` |
| Cover normalisation and cache | `E:621-830` | 260x390 fill-crop PNG, sha1 key, formats PNG/JPEG/WEBP/BMP/DDS | `library/CoverCache` (`QImage` + `QImageReader`; DDS needs a plugin or own decoder) |
| Built Images scan and batch upload | `E:12856-13075`, `ui/tab_images.py` | Extension list, `-bp` badge, sequential direct upload | `library/ImageScanner`, `ui/BuiltImagesPage` |
| Filename and dump naming | `E:275-530`, `E:23012-23330` | Sanitiser incl. cmd metachars, name template | `core/NameSanitizer`, `library/DumpRenamer` |
| i18n | `E:977-2243`, `ui/tab_language.py` | English-as-key dict, 17 languages, 126 keys, in-place retranslation | `i18n/Translator` (`QTranslator` fed from generated `.ts/.qm`) + `ui/LanguagePage`; migrate dict to `.ts` |
| Updater | `E:26287-26730` | GitHub latest release, `.exe` + `.sha256` fail-closed, batch swap with rollback | `update/UpdateChecker` (`QNetworkAccessManager`, `QCryptographicHash`) + platform helper for swap |
| Help, tutorial fetch | `E:21820-21925`, `ui/tab_help.py` | Bundled HTML is stale (v1.7.1); optional tkinterweb | `ui/HelpPage` (`QTextBrowser` or QtWebEngine; refresh bundled content) |
| Credits, donors, GitHub download count | `ui/tab_about.py` | `donors.json` + built-in fallback; releases API | `ui/CreditsPage`, `update/GithubStats` |
| Report issue and diagnostic file | `ui/tab_report.py`, `E:3460` | Local file only, redacted settings | `diag/ReportBuilder`, `ui/ReportPage` |
| Release modal and support prompts | `ui/release_notes.py` | Per-version gate, op counter | `ui/ReleaseNotesDialog` |
| Notifications, sound, Discord, shutdown | `E:26177-26233`, `E:20256-20335` | `winsound`, PowerShell toast, webhook POST, shutdown/restart/sleep countdown | `platform/Notifier` (`QSystemTrayIcon`, `QSoundEffect`), `platform/PowerActions` |
| Theme tokens and fonts | `tkinter_theme.py` | Single dark theme, `px()` DPI scaling | `ui/Theme` (QSS from tokens), `QFontDatabase` |
| Icons | `ui/icons.py`, `ui/icons_data.py` | 177 named Lucide masks recoloured per state | `ui/IconProvider` (SVG set + tinting) |
| Keyboard shortcuts | `E:4317`, `E:4094-4235` | Ctrl+B, Esc, Ctrl+Q real; F5 and Ctrl+Shift+T dead | `app/Shortcuts` (implement the 3 working ones; decide on the 2 dead ones) |
| Edit > exFAT File Manager | `ui/tab_files.py`, `E:8872-9340` | OSFMount mount + drive letter; engine doc elsewhere | `ui/EditExfatMode` -> engine from LEGACY_INVENTORY.md |
| Build/Extract/Convert/Edit(ffpkg,pfs)/Backports | see P20 | Out of scope here | see LEGACY_INVENTORY.md, FPKG_PARITY.md, BACKPORT_PARITY.md |

### 9.1 Legacy defects and traps to decide on (not to copy blindly)

| # | Finding | Evidence |
|---|---|---|
| 1 | `F5` and `Ctrl+Shift+T` shortcuts are listed in the shortcuts dialog but call undefined methods | `E:4134-4145`, `E:4126-4132` |
| 2 | PS5 status ping reads `ps5_ip` which is never saved | `E:26869` |
| 3 | Payload record `ip`/`port`/`last_sent` are displayed but not used/written | `ui/tab_payloads.py` detail pane, `E:18736` |
| 4 | Klog: unbounded buffer; Resume does not replay | `E:19160-19178` |
| 5 | Manager hero tiles Firmware / etaHEN / Temperature never set | `ui/tab_ps5_mgr.py:639-645` |
| 6 | "FW 4.51+" chip in Built Images does nothing | `ui/tab_images.py` filter |
| 7 | Language Import button is a stub; the Language picker lists 18 rows for 17 languages | `ui/tab_language.py:446-458` |
| 8 | Only 62 of 1 559 translatable literals have translations; many pages hard-code English | AST count |
| 9 | Library "Build" / "Build All" use the legacy queue and `_switch_tab('build')` | `E:22853-22890`, `E:13144-13183` |
| 10 | Hidden settings with no UI: `discord_webhook`, `lib_auto_scan`, `auto_ip` | `E:4351`, `E:4495`, `E:4357` |
| 11 | Quick-upload path (`_ftptab_*`, `_ftp_upload_file`) leaves a partial remote file on cancel; the queue path cleans it | `E:9975-10130`, `E:26085-26110` |
| 12 | Bundled help HTML is v1.7.1-era; `tkinterweb` not pinned in `requirements-build.txt` | `E:79`, `requirements-build.txt` |
| 13 | Payload default port is 9090 in the Payloads page but 9021 for ShadowMount+/MicroMount; both texts say "9090 / 9021" | `E:18478`, `ui/tab_shadowmount.py:3158` |
| 14 | History Duration column is a constant `—`; no duration is stored | `ui/tab_history.py:538`, `E:12102-12135` |
| 15 | Y2JB IP field is the same setting as FTP IP (`ftp_ip`), saved by Y2JB "Save Connection Settings" | `E:20578-20583` |

---

## 10. Counts

Counts are of rows in the control tables of section 2 (IDs of the form `XXX-n`) plus the structural counts quoted above. Rows that bundle several widgets (for example `SMP-28`, `ADV-20`) count as one, so the true widget total is higher.

| Metric | Count |
|---|---|
| Sidebar pages | 14 (+ header Help/Settings shortcuts) |
| Registry keys | 25 (11 legacy/hidden) |
| Sub-tabs / modes / rail panes enumerated | Library 2, PS5 7, Settings 3 (+7 rail panes), Advanced 5, Backports 5, Edit 3 modes, Y2JB 5, FTP 2 views, ShadowMount+ 8 rail entries, MicroMount 5 rail entries |
| Pages specified in detail (P01-P19) | 19 |
| Control rows (IDs) | 230 |
| Languages | 17 (16 translation tables + English source); 18 picker rows |
| Strings | 126 unique translation keys; 117 per language (zh 126); 1 881 table entries; 2 066 `_()` calls (1 559 unique literals, 62 present in tables) |
| Icons | 177 |
| Settings keys (literals found) | 110 |

## 11. UNVERIFIED register

| # | Item |
|---|---|
| U1 | Everything about runtime behaviour: the app was not run (Windows-only, Tk). Findings are from code reading |
| U2 | Where `_switch_tab('build')` lands for Library/History call sites (legacy frame vs unified Build) |
| U3 | Whether Library "Build All" feeds the unified Build queue or only the legacy `_queue` |
| U4 | Format of the `SITE AVAIL` reply on real PS5 FTP servers (zftpd, ftpsrv, etaHEN) |
| U5 | Behaviour/ports of third-party PS5 services (etaHEN DPI V2 on 12800, klogsrv on 3232, elfldr on 9021, etaHEN FTP on 1337) are taken from UI text and code defaults, not from their sources |
| U6 | MicroMount push/load/send handlers assumed to mirror ShadowMount+ (only structure and key names read) |
| U7 | (resolved: Duration is a literal dash) |
| U8 | Whether `tkinterweb` is present in the shipped exe (Help rendering) |
| U9 | Writers for the PS5 activity kind 'connection' |
| U10 | RTL handling for Arabic |
| U11 | Which naming radio is pre-selected in Dump Rename (code falls back to `ppsa_title_v`; the radio variable initialisation was not read) |
