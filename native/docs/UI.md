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
| `services/AppStores.{h,cpp}` | `LibraryStore` (scan locations + games, `library.json`) and `PayloadStore` (`payloads.json`) under `Paths::dataDir()`. |
| `pages/Page.{h,cpp}` | `Page` (scrollable content under a `PageHeader`, navigation/drop/F5/Ctrl+B/demo-state virtuals) and `JobFormPage` (form + dock template, availability state, submit flow). |
| `pages/*Page.{h,cpp}` | One class per destination (below). |
| `MainWindow.{h,cpp}` | The shell: pages, sidebar, chips, status bar, shortcuts, drag and drop, toasts, tray + beep, after-queue countdown, update banner, crash banner, problem report, `screenshotAllPages`. |
| `models/` | `JobQueueModel`, `LogModel`/`LogFilterModel` (unchanged). |

## 2. Page map

| Sidebar | Key | Class | Template | Job kinds (through `JobSubmit`) |
|---|---|---|---|---|
| Home | `home` | `HomePage` | status strip (console, queue, free space), Continue card, Recent jobs (history) | — (`JobQueue::retry` for Continue) |
| Build image | `build` | `BuildImagePage` | Form + dock | `image.exfat.build`, `image.ufs2.build`, `image.pfs.build` |
| FPKG | `fpkg` | `FpkgPage` | Form + dock (+ Dependencies, Build summary) | `fpkg.build` |
| Extract & verify | `extract` | `ExtractVerifyPage` | Form + dock | `image.<fmt>.extract`, `image.<fmt>.verify`, `fpkg.extract`, `fpkg.verify` (by extension) |
| Backport | `backport` | `BackportPage` | Table + panel | `backport.run`, `backport.restore`, `backport.scan` (or `library.scan`) |
| Queue | `queue` | `QueuePage` | List + detail | `JobQueue` controls (pause/resume/cancel/retry/remove/move/hold/clear), `JobHistory::exportCsv` |
| PS5 connect | `ps5` | `Ps5ConnectPage` | Tabs: FTP browser · Payloads · Kernel log | `ps5.ftp.upload`, `ps5.payload.send`; TCP reachability probe is UI-side |
| Library | `library` | `LibraryPage` | Tabs: Games · Languages | `library.scan`; `tools.languages.scan/strip/restore`; folder rename is local |
| Logs | `logs` | `LogsPage` | Full table | — |
| Settings | `settings` | `SettingsPage` | Sections (generated from `SettingsSchema`) | — (`SettingsStore`, `DependencyRegistry`, `Paths::tempUsageBytes/sweepStaleTemp`) |
| About (footer) | `credits` | `AboutPage` | Cards | `update.check` (Check for updates), `tools.report` (Report a problem…) |

Shell-level jobs: `update.check` three seconds after start when `general.checkForUpdatesOnStart` is on (hidden from the Queue page as a system kind), `update.download` from the update banner, `tools.report` from the help menu / About / crash banner.

Navigation arguments (`Page::navigateWith`): `build {source, format}`, `fpkg {source}`, `extract {source, mode}`, `backport {folder}`, `queue {jobId | "cancel:<id>"}`, `logs {jobId, source, level}`, `settings {section, key, howToFix}`, `library {tab, languagesFolder}`, `ps5 {tab, upload}`.

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
| FTP browser and kernel log | Designed unavailable states; "Send a file to the console" submits `ps5.ftp.upload`; payloads submit `ps5.payload.send` | The connect module is not in this build; the reachability test (TCP connect to the FTP port) is the only UI-side network call. |
| Library/Backport table rows come from a scan job | When `library.scan` / `backport.scan` are absent, folders added by hand or dropped are summarised in-app (`GameInfoReader`) and the direct children of scan locations are listed | Keeps the pages usable before the engines land; the scan jobs replace the list when they exist. |
| Per-page "Activity" wells on every job page | Backport and the Queue detail panel have them; Build/FPKG/Extract rely on the dock | The dock already shows the running job; a second well duplicated it. |
| Table header sort chevrons | Headers are not sortable yet | No table needed sorting for v1. |

## 5. Screenshots and tests

`exFAT Image Builder --screenshot-pages <dir>` (CI) renders every page, the demo states (`build-space-error`, `fpkg-error`, `extract-error`, `queue-cancel-dialog`, `library-languages`, every Settings section, `settings-reset-dialog`), the toast stack, the compact sidebar, the focus ring and the 1100 px layout. Locally: `QT_QPA_PLATFORM=offscreen FW_DATA_DIR=<tmp> …`.

`tests/app/tst_app_ui.cpp` (offscreen): construction, every page opens, keyboard navigation (arrows, Space, Ctrl+L, Ctrl+1), Settings round trip (edit → store → reload, invalid value rejected with message, reset section, dependent row disabled), override badge in `JobOptionsPanel`, Queue page with a fake kind registered through `AppContext::mutableJobFactory()` (enqueue, reorder, cancel, retry, clear), Logs filter, unavailable kind state, screenshots per page.
