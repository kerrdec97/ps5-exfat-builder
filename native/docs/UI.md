# Farewell Edition — native UI (Qt 6 Widgets)

The implementation of `docs/DESIGN_PROPOSAL.md` and the Design canvas artboards
(Main/Build image, Fpkg, Backport, Queue, Settings, Logs, States, Connect).
Everything lives under `src/app/`; tests under `tests/app/`.

## 1. Layout of `src/app/`

| Folder | Contents |
|---|---|
| `ui/Theme.{h,cpp}` | Every token: colours (`theme::color::bg_0` …), spacing, radii, sizes, type styles (`theme::font::title()` …), the generated QSS (`theme::stylesheet()`), `theme::apply(QApplication*)` (fonts, palette, style, effects off). Nobody writes a hex outside this file. |
| `ui/Style.{h,cpp}` | `FwStyle` (QProxyStyle over Fusion): checkbox/radio indicators, tinted chevrons, header arrows, no menu shadows, tooltip delay 450 ms, no animations; `FocusRingOverlay`: the 2 px `focus` ring painted 1 px outside the focused widget. |
| `ui/Icons.{h,cpp}` | Lucide SVGs tinted per state (unchanged) + `initIcons()` (the qrc lives in a static library). |
| `ui/Widgets.{h,cpp}` | Component set: labels/`ElideLabel`/`Link`, `Card` (regular/table/inset, drop highlight), `button()` + variants, `IconButton`, `Badge` (tone → glyph + word + tint), `Switch`, `Segmented`, `ProgressBar` (determinate / 1.4 s indeterminate sweep / done / failed / paused), `ProgressBlock` + `StageStepper`, `InfoBar`, `EmptyState`, `ErrorState`, `UnavailableState`, `PathField`, `MessageRow`, `TwoColumnLayout` (form + 400 px aside, wraps below the breakpoint), `FlowLayout`. |
| `ui/Sheet.{h,cpp}` | `Sheet`: in-window modal dialog over the scrim (480/640 px, safe button focused for destructive dialogs, Esc = safe, Tab trapped); `ToastHost`: bottom-right stack, max 3, 6 s for success, sticky for warnings/errors, hover pauses. |
| `ui/Shell.{h,cpp}` | `NavItem`/`Sidebar` (groups, pill, count badge, compact rail, Up/Down between items), `AppHeader` (brand, status chips, help), `StatusBar` (28 px), `PageHeader`. |
| `ui/SettingRow.{h,cpp}` | `SettingEditor` (one control per `SettingType`), `SettingRow` (label, help, message, disabled reason, override marker + "Use default"), `JobOptionsPanel` (every `perJob` setting of the given sections → overrides map). |
| `ui/DependencyRow.{h,cpp}` | `DependencyRow` + `DependencyList` bound to `DependencyRegistry` (Check again, Copy report, how to fix). |
| `ui/JobRows.{h,cpp}` | `JobRow` (56 px queue / 36 px dock), `JobList` (filtered stack over `JobQueueModel`, drag reorder with a 2 px accent drop line, per-state actions, auto-retry countdown), `JobsDock` (running block + queued rows + footer). |
| `ui/LogView.{h,cpp}` | `LogDelegate`/`LogTable` over `LogFilterModel`; `ActivityWell` (inset log tail per job or source). |
| `services/JobSubmit.{h,cpp}` | The only path from pages to `JobQueue::enqueue`. Job kinds and parameter names are listed at the top of the header. Unregistered kinds return `ErrorCode::Unsupported`. |
| `services/GameInfo.{h,cpp}` | Background summary of a game folder (param.json + tree size) for the Source cards and manual library rows. |
| `services/AppStores.{h,cpp}` | `LibraryStore` (scan locations + games with backport-scan issues, `library.json`) and `PayloadStore` (`payloads.json`) under `Paths::dataDir()`. |
| `services/ConsoleSession.{h,cpp}` | One `ps5::FtpClient` on a `QThread` worker (Qt sockets need a Qt thread): connect, list, delete, rename, mkdir, mount-config load/push, SITE AVAIL; results arrive as queued signals. |
| `services/SetupChecklist.{h,cpp}` | Home's PS5 setup checklist (MGR-5): five rows from the `ps5.*` settings, the last FTP diagnosis (`<data>/ps5-last-check.json`, written when a `ps5.ftp.diagnose` job finishes) and the payload list; each pending row names where to fix it. |
| `services/ImageTree.{h,cpp}` | Reads an .exfat (`ExfatReader`) or .ffpkg (`Ufs2Reader`) tree on a worker thread into a format-neutral node list for the editor. |
| `services/HexPreview.{h,cpp}` | Hex preview bytes: `HexSliceReader` reads one slice of a file inside an image (`integration::readFileSlice`, any image format or a folder) on a worker thread, opening and closing the image per slice so no handle stays on an image a queued edit may replace; `hexDumpLines` formats offset / hex / ASCII lines (16 bytes each). |
| `pages/Page.{h,cpp}` | `Page` (scrollable content under a `PageHeader`, navigation/drop/F5/Ctrl+B/demo-state virtuals) and `JobFormPage` (form + dock template, availability state, submit flow). |
| `pages/*Page.{h,cpp}` | One class per destination (below). |
| `pages/CompareResultSheet.{h,cpp}` | The result of an `image.compare` job (its `JobOutcome::details`): A and B with kind, files, folders and size; a badge per difference kind; the differences grouped (only in A, folders only in A, only in B, folders only in B, different size, different content with the first differing byte, unreadable), "… and N more" past the list limit; notes; Copy report (plain text, like the legacy `.compare.txt`). |
| `pages/HexPreviewSheet.{h,cpp}` | Read-only hex preview of one file in the open image: offset / hex / ASCII (mono, or mono small when 16 bytes would not fit the sheet), the first 64 KiB, Load more appends the next 64 KiB, "Showing bytes 0–n of N", read errors in an inline error bar. |
| `pages/ps5/DiagnosticsTab.{h,cpp}` | PS5 connect › Diagnostics (FTP-18): folder field (follows the FTP browser's open folder until edited), Run diagnostics / Run speed test (`ps5.ftp.diagnose` through `JobSubmit`, Cancel while it runs), verdict badge, result cells (reachable, round trip, login, destination, passive, binary, writable, server type), greeting, FEAT chips, speed card (upload/download MB/s, read-back, leftover warning). Renders any finished diagnose job, also one retried from the Queue. |
| `pages/ps5/FindConsole.{h,cpp}` | "Find my PS5" header button + sheet: `ps5::discoverConsoles()` on a worker (QtConcurrent, cancelled when the sheet closes), progress bar and count, table of servers found (address, ports, greeting, time); *Use this console* writes `ps5.consoleIp` and `ps5.ftpPort`. |
| `pages/DlcEmuEditor.{h,cpp}` | The dlc_emu.ini editor (Library › DLC emulator tab): table of entries, add/duplicate/remove, live validation + byte counter, Find DLC entries (`tools::dlcemu::autoFind` on a worker, review sheet with confidence badges and placeholders), Save with the .bak confirmation. |
| `MainWindow.{h,cpp}` | The shell: pages, sidebar, chips, status bar, shortcuts, drag and drop, toasts, tray + beep, after-queue countdown, update banner, crash banner, problem report, `screenshotAllPages`. |
| `models/` | `JobQueueModel`, `LogModel`/`LogFilterModel` (unchanged). |

## 2. Page map

| Sidebar | Key | Class | Template | Job kinds (through `JobSubmit`) |
|---|---|---|---|---|
| Home | `home` | `HomePage` | status strip (console, queue, free space), PS5 setup checklist ("n / 5 complete", links to PS5 connect › Diagnostics / Payloads and Settings › PS5 connection), Continue card, Recent jobs (history) | — (`JobQueue::retry` for Continue) |
| Build image | `build` | `BuildImagePage` | Form + dock | `image.exfat.build`, `image.ufs2.build`, `image.pfs.build` |
| FPKG | `fpkg` | `FpkgPage` | Form + dock (+ Dependencies, Build summary) | `fpkg.build`, `fpkg.addon` (add-on mode: entitlement key, volume type, "From dlc_emu.ini…"), live CPU limit (`fpkg::setLiveCpuLimit` on running package jobs), existing-package sheet → `fpkg.outputConflict` override |
| Extract & verify | `extract` | `ExtractVerifyPage` | Form + dock (+ Compare card) | `image.<fmt>.extract`, `image.<fmt>.verify`, `fpkg.extract`, `fpkg.verify` (by extension); `image.compare {a, b, depth}` from the Compare card (image A, Image/Folder B, depth "Names, sizes and contents" / "Names and sizes"; validation inline; badge follows the job: Queued / Comparing… / Identical / n differences / Failed; View result opens `CompareResultSheet`, kept after the queue entry is cleared) |
| Edit image | `edit` | `ImageEditorPage` | Form + dock (+ Convert to…) | `image.exfat.edit`, `image.ffpkg.edit` (operations JSON per EXFAT.md / UFS2.md), `image.convert`; tree context menu (Preview…, Replace…, Rename, Delete) and double-click / Enter on a file → `HexPreviewSheet` (no job: `HexSliceReader`) |
| Backport | `backport` | `BackportPage` | Table + panel | `backport.run` (one job per game), `backport.restore {game, zip}` / `{journal}` (start-up banner from `backport::interruptedApplies()`), `backport.scan` (per-game issues/canRun/plan shown as status badge + tooltip) |
| Queue | `queue` | `QueuePage` | List + detail | `JobQueue` controls (pause/resume/cancel/retry/remove/move/hold/clear), `JobHistory::exportCsv` |
| PS5 connect | `ps5` | `Ps5ConnectPage` | Header: address, FTP port, Find my PS5, Connect. Tabs: FTP browser · Payloads · Kernel log · Y2JB · Mount config · Diagnostics | `ps5.ftp.diagnose` (Diagnostics tab), `ps5::discoverConsoles` (Find my PS5); FTP session (`ConsoleSession`): browse, breadcrumbs, delete/rename/mkdir with the protected-path sheet; `ps5.ftp.upload` (+ `allowProtectedPaths`), `ps5.ftp.download`, `ps5.payload.send` (target port from the ps5.* keys), `KlogClient` viewer (bounded `KlogModel`, tags, filter, pause, export), `ps5.dpi.install`, `ps5.y2jb.autoBlock` (confirm sheet, region), local Y2JB patching (`patchLocalFolder` on a worker), ShadowMount+/MicroMount config (`MountConfig` parse/render/validate, load/push over the session), `ps5.shadowmount.stop` |
| Library | `library` | `LibraryPage` | Tabs: Games · Languages · DLC emulator | `library.scan` (covers from `coverPath`, platform/kind), `library.rename` (preview sheet → apply with edited names → undo), `tools.languages.scan/strip/restore`, `tools::dlcemu` editor |
| Logs | `logs` | `LogsPage` | Full table | — |
| Settings | `settings` | `SettingsPage` | Sections (generated from `SettingsSchema`) | — (`SettingsStore`, `DependencyRegistry`, `Paths::tempUsageBytes/sweepStaleTemp`) |
| About (footer) | `credits` | `AboutPage` | Cards | `update.check` (Check for updates), `tools.report` (Report a problem…) |

Shell-level notifications: when the queue finishes, `QueueCompletionWatcher` drives the tray message (`general.notifyOnFinish`), the after-queue countdown and the Discord webhook (`notify::DiscordNotifier`, `general.discordWebhookUrl`; update checks and FTP diagnostics are left out of the message; a failed delivery shows a warning toast).

Shell-level jobs: `update.check` three seconds after start when `general.checkForUpdatesOnStart` is on (hidden from the Queue page, the Jobs dock, the header chip and the status bar as a system kind; `jobs::isSystemKind`), `update.download` from the update banner, `tools.report` from the help menu / About / crash banner.

Navigation arguments (`Page::navigateWith`): `build {source, format}`, `fpkg {source}`, `extract {source, mode}`, `edit {image}`, `backport {folder}`, `queue {jobId | "cancel:<id>"}`, `logs {jobId, source, level}`, `settings {section, key, howToFix}`, `library {tab, languagesFolder, dlcFolder}`, `ps5 {tab (key: ftp, payloads, klog, y2jb, mount, diagnostics), upload}`.

## 3. How the schema drives the UI

* Settings page: sections = `SettingsSchema::sections()` in `sectionOrder()` (+ Tools when dependencies exist); cards per `group`; `advanced` rows behind "Show advanced"; `restartRequired` adds "Takes effect after a restart"; `enabledWhenKey` disables the row with a reason; `normalize()` errors show under the control and are not saved; "n settings differ" + Reset section; Reset all… confirms; Import legacy settings… calls `SettingsStore::importLegacy`. The Tools section lists every `DependencyRegistry` entry; other sections list the dependencies their settings name (`dependencyId`). Storage shows the temporary-files row.
* Job pages: `JobOptionsPanel` lists the `perJob` settings of the relevant sections (Build image: Build/Verification/Performance/Storage/PFS; FPKG: FPKG/PlayGo/Firmware and SDK + Verification/Performance/Storage; Extract: Verification/Performance; Backport: Backport). A changed value shows "Overridden for this job" with "Default is X. Use default"; the panel returns the overrides map for `enqueue`.
* Queue detail panel: path-like parameters and the job's overrides ("Overridden" badge), attempt number, auto-retry countdown ("Retrying in 25 s · attempt 2 of 3").

## 4. Deviations from the proposal, with reasons

| Proposal | Implementation | Why |
|---|---|---|
| Focus ring painted by each widget 1 px outside its bounds | One transparent `FocusRingOverlay` over the window paints the ring around the focused widget | A Qt widget cannot paint outside its own geometry; the overlay keeps the exact spec (2 px, radius + 1, outside) for every control including QSS-styled ones. Inputs ring on any focus, other controls after keyboard focus (`WA_KeyboardFocusChange`). |
| Dialogs as frameless `QDialog`s | `Sheet`: a child widget of the main window with its own event loop | Captured in screenshots, never gets a native title bar, and the scrim covers exactly the window. |
| Sidebar `menu` icon, `logs` icon, `inbox` empty-state icon | `sidebar`, `file_text`, `queue` | Those names do not exist in the shipped 195-icon set. |
| Settings sections fixed list of 13 | Only sections that have settings (or dependencies) are listed | Generated from the schema; absent modules add no empty sections. |
| `ui.lastPage`, `ui.sidebarCompact` in the settings store | QSettings (`window/geometry`, `window/sidebarCompact`, `window/lastPage`, `crash/lastAcknowledged`) | Window state is not a job setting; the schema is owned by modules. |
| Queue "finishes in about 24 min" | Shown as the running job's time left plus the queued count | Queued jobs have no estimate until they run. |
| Edit folds into Extract & verify | Own "Edit image" page under Workflows (icon `edit`) | Browsing a tree, staging operations and reviewing them is a different template from the extract form; Convert to… lives on the same page. |
| Mount config list keys as multi-line | Shown as one row per key; list values separated with `;` in the table | A table cell cannot edit line breaks; the page converts back to one repeated key per line before rendering. |
| Library/Backport table rows come from a scan job | Also true without the engines: hand-added or dropped folders are summarised in-app (`GameInfoReader`) | The scan jobs replace the list when they run. |
| Per-page "Activity" wells on every job page | Backport and the Queue detail panel have them; Build/FPKG/Extract rely on the dock | The dock already shows the running job; a second well duplicated it. |
| Table header sort chevrons | Headers are not sortable yet | No table needed sorting for v1. |

## 5. Screenshots and tests

`exFAT Image Builder --screenshot-pages <dir>` (CI) renders every page, the demo states (`build-space-error`, `fpkg-error`, `extract-error`, `extract-compare` (the Compare card with a result), `extract-compare-result` (the result sheet), `edit-hex-preview` (the preview sheet with sample bytes), `queue-cancel-dialog`, `library-languages`, `ps5-diagnostics` and `ps5-find` (specimen results, labelled as such), every Settings section, `settings-reset-dialog`), the toast stack, the compact sidebar, the focus ring and the 1100 px layout. Locally: `QT_QPA_PLATFORM=offscreen FW_DATA_DIR=<tmp> …`.

`tests/app/tst_app_ui.cpp` (offscreen, 15 cases): construction, every page opens, keyboard navigation (arrows, Space, Ctrl+L, Ctrl+1), Settings round trip (edit → store → reload, invalid value rejected with message, reset section, dependent row disabled), override badge in `JobOptionsPanel`, Queue page with a fake kind registered through `AppContext::mutableJobFactory()` (enqueue, reorder, cancel, retry, clear), Logs filter, unavailable kind state, image editor (staged operations JSON + a real exFAT image built by the engine and browsed), bounded kernel-log model, FTP browser against the in-process `fake::FakeFtpServer`, dlc_emu editor round trip and validation, `library.rename` preview through the real engine, screenshots per page.

`tests/app/tst_image_tools.cpp` (offscreen, against .exfat and .ffpkg images built by the engines): `hexDumpLines` format and offset width, `readHexSlice` / `HexSliceReader` bytes equal the source file (first slice, last partial slice, missing path), Edit image › preview of a file with Load more to the end of the file, the Compare card (validation queues nothing; identical images → "Identical"; image vs an edited folder → "3 differences", grouped result sheet and report; the result survives clearing the queue), the report's "… and N more" for capped lists.

`tests/app/tst_ps5_tools_ui.cpp` (offscreen, 6 cases): Find my PS5 against the fake FTP server on 127.0.0.1 (sheet driven, *Use this console* → `ps5.consoleIp`/`ps5.ftpPort` and the header fields), cancelling a search changes nothing, the Diagnostics tab running the real `ps5.ftp.diagnose` job (checks, then the 16 MiB speed test; cells, verdict, files removed), an unreachable console, the Home checklist counting 0 → 5 of 5 (address, port clash, payload file, browser connected, the remembered diagnosis) and its fix link opening PS5 connect › Diagnostics (tab key `diagnostics`), the Discord webhook setting.
