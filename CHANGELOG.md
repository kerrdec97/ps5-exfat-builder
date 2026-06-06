# Changelog

## [3.6.0] - Unreleased

### Added
- **CPU core selection on the Build tab** — a "CPU cores" dropdown in
  the Configure-output card (values scale to the machine's core
  count). It drives the same robocopy /MT threads setting as
  Advanced → Build Parameters → "Threads (safe)", so the two controls
  stay in sync; picking a value clamps (1–128) and saves immediately.
- **What's-new dialog on launch** — a Steam-style release screen:
  MAJOR UI REFRESH hero, four stat counters (7 tabs / 12 UI
  improvements / 3 features / 100% compatible), three real screenshot
  highlight cards (Backports, Advanced, Settings — bundled under
  assets/whatsnew/), a What's-Improved checklist, a community
  supporters banner, and an integrated Support / Open Changelog /
  Let's Build footer. The amber PFS testing-phase notice is kept. The
  separate close-time Ko-fi popup is also kept (it fires on exit, not
  alongside this dialog); it can be removed in one line if preferred.
- **Support popup on exit** — a small thank-you dialog with the Ko-fi
  link (ko-fi.com/deckerr9746220). Either button continues the normal
  close path (settings save, temp cleanup, mount sweep).

### Changed
- **Credits — supporters**: the Ko-fi donations total was removed; the
  panel now simply thanks donors. Total downloads is fetched live from
  the GitHub releases API (the same figures as the github-release-stats
  page), with the optional donors.json value as the offline fallback.
  The "Support the tool" button now opens the project Ko-fi page.

### Fixed — Advanced pane scrolling + layout (from screenshot feedback)
- **Scroll bug (root cause)**: the three Advanced sub-tab panes
  (Build Parameters / Post-Build / Language Stripper) built directly
  into non-scrolling frames, so content taller than the viewport was
  clipped — most visibly Post-Build's Danger Zone falling off the
  bottom. All three panes now build inside a scroll host whose
  scrollregion tracks the content height, so nothing is unreachable
  and future cards can't push content off-screen.
- **Build Parameters layout**: the cramped 300px right sidebar (which
  wrapped text and unbalanced the cards at higher Windows scaling) is
  gone. The four cards now sit in a full-width 2x2 grid with a
  full-width Recommendation Assistant beneath them — Recommended setup
  / Current build profile / Why these defaults across three roomy
  columns.
- **Post-Build**: the Automation Summary became a compact Automation
  Pipeline — the checklist and the Build → Verify → Log → Upload flow
  now hug the left instead of stranding the flow in empty space,
  reclaiming ~100px of vertical room.

### Fixed — Build Parameters layout (from live screenshot feedback)
- **Text clipping**: the forced-uniform 2x2 card rows squeezed the
  Filesystem card (cutting the "Locked. 4096…" help text) while
  stretching the ffpkg card with dead space. Rows now size to content
  and pairs equalize naturally.
- **Card balance**: the three ffpkg rows (block size, fragment, locked
  sector) moved from Filesystem layout into the ffpkg parameters card
  where they belong — Filesystem/Image pair on top, Performance/ffpkg
  pair below, all four visually balanced.
- **Alignment**: Write speed is now a proper label/control row aligned
  with the entries above it; int/text entries share one width.
- **Toggle status** chips became real pill badges (✓ Enabled /
  ○ Disabled).
- **Sidebar**: the Recommended card's border now matches the center
  cards; "Why these defaults?" copy shortened so the sidebar fits
  without clipping.
- **Typography**: larger page title and richer subtitle, bigger card
  titles/subtitles and row labels, more breathing room between rows.

### Changed — Advanced & Settings premiumization pass
- **Build Parameters** side rail gained compact hardware stat cells
  (Threads / Sector / Performance / Health, from the existing drive
  detection) inside the Recommended card, plus a **Current Build
  Profile** card mirroring Cluster / Sector / Threads / Retries /
  Bytes-inode live with per-row status dots and an overall OPTIMAL /
  CUSTOM / RISKY pill (red = the known-risky 4096 sector).
- **Post-Build** gained a full-width **Automation Summary**: a ✓/✗
  enabled-actions checklist and a Build → Verify → Log → Upload
  workflow strip whose steps light up as the matching toggles are
  enabled. The Danger Zone got a red header band and a DESTRUCTIVE
  badge. Every toggle row across Advanced now shows a live Enabled /
  Disabled status chip next to the control.
- **Language Stripper** summary gained a Selected cell and a live
  "Keep: …" line (both read from the future scan stats dict).
- **Settings**: OSFMount summary gained a Detected Path cell; Logs
  shows a premium "No logs available" empty state; Theme now uses the
  preview cards exclusively (the redundant radio row was retired —
  same var, same toggle path); Auto-Shutdown gained a live
  Trigger → Wait → Action workflow preview; SettingsCard headers got
  larger titles and more breathing room app-wide.

### Changed — Advanced & Settings dashboard pass
- **Advanced → Post-Build** became an automation dashboard: Upload /
  Verification / Logging / Image Output cards in a grid plus a
  full-width red-bordered **Danger Zone** for the delete-source
  toggles. Every card carries a live ON / OFF / ARMED state pill; the
  six setting rows keep their exact vars and callbacks.
- **Advanced → Language Stripper** became a feature page: top summary
  strip (Selected Folder — live, Languages Found, Estimated Saving),
  bottom summary (Space To Save / Files To Remove / New Estimated
  Size), and a large primary "Strip Selected Languages" action. The
  scan-derived cells read `app._adv_lang_stats` and show "—" until a
  scan backend populates it (none exists in this build yet).
- **Settings → Logs** became a dashboard: Total Logs / Total Size /
  Failed / Last Log cells plus a Recent Logs table (newest six,
  double-click opens the file), refreshed automatically after builds
  and Clear all logs. Open / Clear buttons unchanged.
- **Settings → Theme** gained two selectable preview cards (Dark
  Purple / Light) with mini window mockups; clicking one drives the
  same `_theme_var` + `_toggle_theme` path as the radios below.
- **Settings → PS5 FTP** summary strip gained an **Auto Upload** cell
  mirroring the pane's auto-upload-after-build toggle.
- `ui/tab_settings.py` now imports `os` explicitly instead of relying
  on the star-import.

### Changed — Backports / Convert / Dump Rename / History / Advanced / Settings / Credits presentation pass
Second presentation pass (after the PS5 section below) bringing the
remaining seven tabs to the Build-tab visual language, matched against
the v3.6/v3.7 design boards. No backend, workflow, pipeline, or
settings-storage logic was touched.

- **Backports — Games** cards now carry the SDK badge on the cover,
  the dump size and date, the source folder, and a per-card action row
  (Queue toggle, Process, ⋮ menu with open-folder). A bottom stats
  strip shows Games / Queued / Completed / Estimated queue time.
  Size and date are computed asynchronously alongside the existing SDK
  detection — display only.
- **Backports — Auto Backport**'s selected-game banner became a full
  hero card: cover, title/PPSA, and a live Detected SDK / Target
  Firmware / Patches / Status strip with a READY TO PATCH badge fed by
  the existing detection vars. The form below is unchanged.
- **Backports — Results** entries became status-edged cards (cover
  tile, PPSA chip, zip chips with NOT FOUND state, Open Folder / Open
  ZIP) with All / Worked / Failed filter pills and live counts. The
  It Worked / Didn't Work workflow and its file handling are untouched.
- **Convert** gains a selected-image hero (game parsed from the
  filename, source → output format, size, READY TO CONVERT badge) and
  compact .exfat → .ffpkg flow chips on both cards.
- **Dump Rename** gains a four-cell stats strip (Dumps Found / Matched
  / Unknown / Already Named), a per-row outcome chip (✓ Will rename /
  ⚠ No match / ✓ Already OK), and the Rename button now shows the live
  selection count.
- **History** rows gained a small async cover thumbnail next to the
  game title, matching the Library cards.
- **Advanced**'s "Recommended for your setup" card got a stronger
  accent border.
- **Settings** sections gained dashboard summary strips above the
  controls: OSFMount (version, detected status, last checked,
  architecture — read from the executable itself), Build (temp usage,
  retries, retry wait, app version), Logs (file count, folder), PS5
  FTP (IP, port, reachability), Notifications (sound state), Theme
  (current theme + palette preview), and Auto-Shutdown (action, delay,
  active rules).
- **Credits** became a dashboard: identity hero with icon tile,
  contributor tile cards, and a supporters panel with a stats strip
  (total supporters, Ko-fi total, downloads — the latter two read from
  optional `kofi_total` / `total_downloads` keys in donors.json and
  show "—" until added). Donor loading is unchanged.

### Changed — PS5 section presentation pass
Pure presentation pass bringing the whole PS5 section in line with the
Build-tab visual language (hero cards, stat strips, library cards, shared
empty states). No protocol, transfer, or workflow logic was touched.

- **ShadowMount+ / MicroMount** now land on an **Overview dashboard**:
  one summary tile per configuration section showing live values from
  the editor rows (click a tile to open the existing editor), plus a
  Recent Activity panel (last payload send / config load / config push
  / log fetch, recorded from the existing status line). An Overview
  row was added to the section rail; the editors, dirty tracking,
  save bar, and search are untouched. The ShadowMount+ hero also
  gained a per-image **Rules** count cell.
- **Y2JB** gained a deployment hero strip (PS5 IP / FTP port / DPI
  port / live install state), a Quick Actions card (Browse PS5,
  Upload Package, Open etaHEN folder — each jumps to the existing FTP
  sub-tabs), and an Install Queue panel mirroring the live install
  progress with a session activity feed and last-installed time.
- **Payloads** gained a payload-bus metrics strip (payload count,
  console IP, default port, last-sent time recorded from the status
  line). Per-card Send buttons already existed and are unchanged.
- **PS5 Manager** is now a "PS5 Control Center": hero card with live stats
  (IP, FTP, storage used/free, games on PS5, local images) fed by the
  existing refresh, plus a quick-actions row that jumps to the other PS5
  sub-tabs. Firmware / etaHEN / temperature cells are present but show "—"
  (no data source in the current refresh worker — wired for a future pass).
- **FTP — Quick Upload** is now a transfer dashboard: connection / target
  path / upload-state strip, a connection-information card, and the upload
  form, progress, and log split into discrete cards.
- **FTP — PS5 Browser** gains a stats strip above the dual panes
  (connection, current path, files listed, selection, upload queue), and
  the transfers panel summary now reflects active/cleared state.
- **Klog Monitor** gains a live stats strip (total lines, lines/sec,
  warnings, errors, connection) derived from the existing line buffer via
  a 1-second UI poll — the stream socket and parser are untouched.
- **Payloads** list rows became spaced library cards (file chip, port,
  per-payload IP, last-sent) and the detail pane gained a READY/MISSING
  badge, effective port, and last-sent facts. "Last sent" displays "—"
  until a future pass records it (recording would be a logic change).
- **Y2JB — Install** replaced the stacked region fields with three
  side-by-side USA / EU / JP region cards (PPSA id, patchable badge, PKG
  picker, readiness status, install button). Same DPI install workflow.
- **ShadowMount+ / MicroMount** toolbars became hero cards with config
  stats (sections, port, config source, live status; MicroMount also
  mirrors target dir / scan paths / interval from the form) and the same
  action buttons. The settings editors below are untouched.

## [3.5.3] - 2026-06-05

First release since 3.4.0. The headline is PFS / MicroMount support — none
of it existed in 3.4.0, so the entire PFS tab and its workflows are new to
anyone updating from the public build.

### Added — PFS / MicroMount
- New **PFS** tab with three workflows: build a dump folder into a PFS image,
  convert an existing image, and extract a PFS image back to a folder.
- **Build**: pack a PS5 game dump into a mountable PFS, with an output-format
  toggle — `.ffpfsc` (compressed container, smaller on disk) or `.ffpfs`
  (uncompressed, full console read speed, single fast pass, no temp file).
- **Convert**: turn an existing `.exfat` or `.ffpkg` straight into a `.ffpfsc`
  in one `mkpfs pack file` — no extraction, no OSFMount.
- **Extract**: unpack a `.ffpfs` or `.ffpfsc` to a folder.
- `mkpfs` bundled and run in-process (works in the frozen exe), with a build
  queue, live progress, and CPU-core selection.

### Fixed
- **Containers now mount on ShadowMount+/MicroMount.** The nested image was
  written as `.pfs_image.dat` (leading dot); the mounter only recognises a
  nested PFS named exactly `pfs_image.dat`, so earlier containers mounted
  empty. Any `.ffpfsc` built before this must be re-packed.
- Nested `.exfat` / `.ffpkg` images keep their extension so the mounter
  recognises them.
- Removed mkpfs `--verify` post-create read-back — builds are roughly twice as
  fast, with byte-identical output.

### Changed
- Convert no longer extracts first (was OSFMount + robocopy for exFAT, UFS2Tool
  for ffpkg) — both formats now pack directly.
- Mounts left open (file manager, ffpkg editor, a build) are dismounted
  automatically and safely when the app closes.
- Removed an orphaned PFS-cards module.
- Extract guidance clarified: an uncompressed `.ffpfs` unpacks straight to game
  files, while a `.ffpfsc` unpacks to its nested image.
