# v3.1.0 — Per-game custom image size

The headline of this release: **set a custom image size for any
individual game in your Build queue**, directly from the Build
screen. Useful when one game's auto-computed size is too small
for the exFAT filesystem overhead (Gear Club Unlimited being the
canonical offender) and the build fails with "no space left on
device".

## ✨ What's new

- **📏 Size pill on every queued row.** Inline, visible, no
  right-clicking required. Outlined when unset, filled when a
  custom size is active.
- **Click the pill** → dialog with the source folder size, file
  count, a suggested override, and quick-pick buttons for +1 GB
  / +2 GB / +5 GB bumps.
- **Accepts PowerShell-style input:** `24`, `24G`, `24 GB`,
  `24gb` all parse to 24.
- **Per-item override beats the global Advanced setting** at
  build time. Both build paths log which one was used.
- **Queue row layout fix.** Long game titles no longer push the
  right-side buttons (×, Open folder, Upload to PS5, Size)
  off-screen.
- **Auto-updater hardened** against the "Failed to load Python
  DLL" error some users saw on v3.0.0. SHA256-verifies the
  download, backs up the working .exe before swap, and
  auto-rolls back if the new version fails to launch. See
  `UPDATE_TROUBLESHOOTING.md` if you got stuck on v3.0.0 —
  there's a `.bak` you can rename back.

## 🎮 How to use

1. Add games to the Build queue as usual.
2. On the row for the game that needs a bigger image, click
   **📏 Size**.
3. Pick a size — Suggested, +1 GB, or type your own.
4. Click Apply. The pill turns into a solid **📏 24 GB** badge.
5. Hit Build All. That one game builds at the custom size;
   every other game builds at its own auto-size.

## 📦 Downloads

- **`exFAT Image Builder.exe`** — built artifact, no Python
  install required. Run as administrator.
- **Source zip** — if you want to run from source or audit the
  changes.

## 🔧 Upgrading from v3.0.0

Drop the new files over your existing v3.0.0 install. The header
will read `v3.1.0` on next launch. No settings migration needed.

## 📋 Full changelog

See [`CHANGES.md`](./CHANGES.md) for the detailed notes.

## 💬 Found a bug?

Open an issue with:
- Which game you were building
- The `[INFO]` line from OUTPUT LOG showing which size override
  path was used (per-item vs global)
- The final size of the produced `.exfat` file
- Whether the build verified or failed

## 🙏 Credits

- BestPig for [BackPork](https://github.com/BestPig/BackPork)
  and [PS5-Backports](https://github.com/BestPig/PS5-Backports)
- SvenGDK for UFS2Tool and the SQL gist
- NookieAI and stonemodder (Porkfolio) for the original
  workflow this tool automates
- Everyone in the testing channel who flagged the Gear Club
  Unlimited sizing issue
