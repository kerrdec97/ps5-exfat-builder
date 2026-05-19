# exFAT Image Builder — v3.1.0

**Released:** May 2026
**Previous public release:** v3.0.0 (May 17, 2026 — Apply Backport, Convert ffpkg↔exFAT, rich progress dialog, Y2JB patch verification)
**Status:** ✅ Stable

---

## 🎯 What's new since v3.0.0

Two headline changes in v3.1.0:

1. **Per-queue-item custom image size** with a redesigned inline
   control that's visible on every row — no right-click, no
   Advanced settings, no PowerShell workarounds. Fixes the
   long-standing Gear Club Unlimited "no space left on device"
   build failure.
2. **Auto-updater hardened** against the "Failed to load Python
   DLL" error a few users hit after v3.0.0's update. SHA256
   verification, automatic backup of the working .exe before
   swap, and auto-rollback if the new version fails to launch.

Plus small UI polish that came out of testing.

---

## 🛡 Auto-updater hardening

A few users on v3.0.0 hit this PyInstaller error after auto-update:

```
Failed to load Python DLL
'C:\Users\...\AppData\Local\Temp\_MEI196362\python314.dll'.
LoadLibrary: The specified module could not be found.
```

This is a known PyInstaller-onefile failure mode. The error
message is misleading — Windows is reporting that **a dependency
of** `python314.dll` couldn't be loaded, not the DLL itself.
Three things commonly cause it:

1. **Antivirus quarantining part of the bundle mid-extraction.**
   Windows Defender sometimes grabs `vcruntime140.dll` or
   `_ctypes.pyd` partway through the PyInstaller-onefile
   extraction to `%LOCALAPPDATA%\Temp\_MEI...\`, breaking the
   DLL chain.
2. **The old version's `_MEI` temp folder still mapped** when
   the new version tried to extract its own. The v3.0.0 updater
   waited a flat 4 seconds before swapping, which wasn't always
   enough.
3. **Truncated download.** A connection drop mid-download left
   a partial .exe; the updater happily moved it into place and
   launched it.

The v3.1.0 auto-updater defends against all three:

- **SHA256 verification** of the download against a `.sha256`
  companion asset on the release (if present). Mismatch → abort,
  no swap. Future releases should ship the .sha256 alongside
  the .exe for full integrity protection.
- **Size sanity check** — rejects truncated downloads and
  anything under 5 MB (a real build is ~10+ MB).
- **Backup before swap** — the working .exe is copied to
  `<exe>.bak` before being overwritten.
- **Polled wait for the old process** to release the .exe (up
  to 15 seconds, one-second steps). Probes the file each tick;
  once it's writable, the old process is fully gone and its
  `_MEI` is unmapped. Fixes the race condition that produced
  the DLL error.
- **Post-launch verification** — 6 seconds after launching the
  new .exe, the updater batch uses `tasklist` to confirm it's
  still alive. **If not, it auto-rolls back** to the `.bak`
  and launches the previous working version. The user gets a
  working install back without doing anything manual.
- **`.bak` cleanup** after 30 seconds of successful runtime.

### If you already hit the error on v3.0.0

See `UPDATE_TROUBLESHOOTING.md` in the release zip. Short
version: look in the install folder for
`exFAT Image Builder.exe.bak`. Delete the broken `.exe`, rename
`.bak` → `.exe`, you're back to your previous working version.
(If no `.bak` exists because v3.0.0 didn't create one, download
the v3.1.0 .exe manually from GitHub.)

### Bigger fix on the roadmap

The actual root-cause fix is to ship the app as PyInstaller
`--onedir` instead of `--onefile`. Onedir ships the .exe plus a
`_internal\` folder next to it — no extraction at launch, no
`_MEI` race, no AV scanning a freshly-written DLL chain. The
tradeoff is users see a folder of files instead of a single .exe.
If we keep seeing this error report after v3.1.0, that's the
next step.

---

## 📏 Per-item custom image size

The Advanced settings already had a global "Image size override
(GB)" field, but it forced the same size on every queued build —
useless when only *one* game needs a bump. **Gear Club Unlimited**
(PPSA05027) is the canonical example: its auto-computed size lands
a hair too small for the exFAT filesystem overhead, and the build
fails partway through with "no space left on device". The
workaround used to be running `New-OsfExfatImage.ps1 -Size 24G`
manually from PowerShell.

v3.1.0 adds per-item size override directly to the Build screen:

- Every queued item that's `waiting` or `failed` shows an inline
  **📏 Size** pill on the right side of its row, next to the
  remove (×) button. **No need to discover the right-click menu**
  — the option is visible the moment the item appears in the
  queue.
- The pill is reserved its own space in the row layout. Long
  game titles like "Digimon Story Time Stranger v01.000.000" used
  to push the right-side buttons off-screen — they don't any
  more.
- **Unset state** is an accent-ringed outlined pill (purple
  outline, transparent interior, accent-coloured "📏 Size" text).
  Consistently visible on every row, regardless of alternating
  row colours.
- **Override-set state** is a solid filled accent pill ("📏 24
  GB") in bold. You can see at a glance which rows have overrides
  and which don't.
- Clicking the pill opens a dialog showing the source folder's
  measured size, file count, and a suggested override (source ×
  1.10 rounded up to the next whole GB — the same heuristic the
  Convert tab uses for ffpkg → exFAT).
- Quick-pick buttons offer the suggested size, +1 GB, +2 GB,
  +5 GB for one-click bumps.
- The entry accepts the same formats as the PowerShell script
  (`24`, `24G`, `24g`, `24 GB`, `24gb`) so users carrying over
  muscle memory from `-Size 24G` don't have to retrain.
- A **Clear override** button (only shown when an override is
  currently set) reverts the row to auto-size.
- The pill hides once a row is `building` or `done` — size is
  locked in at that point.
- Right-click menu also gains a **📏 Set custom size...** entry
  as a secondary path.
- The per-item override beats the global Advanced setting at
  build time: if both are set, the per-item value wins. The
  build log records which one was used:
  `[INFO] Using per-item image size override: 24 GB`.

### How to use it

1. Add your games to the queue as usual.
2. On the row for the problem game (e.g. Gear Club Unlimited),
   click the **📏 Size** pill.
3. Either click "Suggested", "+1 GB", or type your own value.
4. Hit Apply. The pill turns into a solid **📏 24 GB** badge.
5. Click Build All. That one game builds at 24 GB; every other
   game builds at its own auto-size.

---

## 🐛 Small fixes & polish

These came out of testing v3.0.0 and are bundled into v3.1.0:

- **Queue-row layout fix.** The output-directory label was set to
  `expand=True`, which let it gobble all available horizontal
  space and push the right-side action buttons (×, Open folder,
  Upload to PS5) off-screen for any row with a long game title
  or output filename. The action cluster now packs first in the
  layout so its space is reserved before the expanding labels
  get a turn. Buttons stay on-screen regardless of title length.

- **Tooltip on the Size pill.** Hovering shows
  "Set a custom image size for this build. Useful when the
  auto-size is too small (e.g. Gear Club Unlimited needs +1 GB)."
  when no override is set, or "Custom image size: N GB. Click to
  change or clear." when one is.

- **Build log calls out which size source was used.** Reads
  `[INFO] Using per-item image size override: 24 GB` or
  `[INFO] Using global image size override: 24 GB` so you can
  confirm from the log which override path took effect.

---

## 📦 Files in this release

- `exfat_builder.py` — main entry point (version bumped to
  3.1.0, QueueItem extended with `size_override_gb`, build flow
  prefers per-item override over global Advanced setting,
  `_show_size_override_dialog` + `_count_files` helpers added,
  queue row layout restructured).
- `ui/tab_ffpkg_edit.py` — unchanged from v3.0.0.
- `ui/tab_files.py` — unchanged from v3.0.0.
- `ui/tab_convert.py` — unchanged from v3.0.0.
- `CHANGES.md` — this file.
- `CHANGES_v3.0.0.md` — preserved v3.0.0 release notes.
- `CHANGES_v206_legacy.md` — preserved v2.0.6 sub-version notes.
- `RELEASE.md` — step-by-step instructions for pushing to GitHub.
- `X_STATUS.md` — pre-written social posts.

Drop these into your existing v3.0.0 install, overwriting the
same paths. The version string in the app's header will read
`v3.1.0` on next launch.

---

## 💾 Disk-space requirements (unchanged from v3.0.0)

| Operation | Free space needed (× source image size) |
|---|---|
| Extract image | ~1.0× |
| Apply Backport (ffpkg or exFAT) | **~2.1×** |
| Convert exFAT → ffpkg | ~1.0× |
| Convert ffpkg → exFAT | **~2.1×** |
| Compare ffpkgs | ~2.0× |

These are unchanged in v3.1.0. The per-item size override doesn't
affect the temp-folder math — it only changes the **output**
image size, which goes to your chosen output directory.

---

## 🔗 Links

- Repository: <https://github.com/kerrdec97/ps5-exfat-builder>
- Issue tracker: <https://github.com/kerrdec97/ps5-exfat-builder/issues>
- BackPork (downgrader): <https://github.com/BestPig/BackPork>
- BPS-format backports: <https://github.com/BestPig/PS5-Backports>

---

*— DecKerr97*
