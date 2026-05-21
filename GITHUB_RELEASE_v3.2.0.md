# v3.2.0 — Live backport progress, per-game image size, hardened updater

The first release since v3.0.0. Four substantial improvements,
all driven by real user reports.

## ✨ What's new

### ⏳ Auto Backport no longer looks frozen
On a large game, decrypting and re-signing every `.self` /
`.sprx` / `.prx` can take 10+ minutes. The full-pipeline path
used to run completely silent for that whole time — no log
output, frozen progress bar — so users assumed it had crashed.
Now:
- Pipeline output **streams to the log live**, line by line
- An **elapsed-time heartbeat** posts "… still working — 3m 45s
  elapsed (128 files processed)" every 15 seconds
- The progress bar **creeps steadily** through 20–70%
- The status line **ticks** with elapsed time

### 📏 Per-game custom image size
Set a custom `.exfat` size on any single queued game. Some games
(Gear Club Unlimited being the known one) need a slightly bigger
image than the auto-size produces, or the build fails with "no
space left on device".
- Inline **📏 Size pill** on every `waiting` / `failed` queue row
- Click it → dialog with source size, file count, suggested
  override, and quick-pick +1/+2/+5 GB buttons
- Accepts `24`, `24G`, `24 GB`, `24gb` input
- Per-item override beats the global Advanced setting

### 🛡 Auto-updater hardened
Fixes the "Failed to load Python DLL" error some users hit after
v3.0.0's auto-update. The new updater **SHA256-verifies** the
download, **backs up** the working .exe before swap, and
**auto-rolls-back** if the new version fails to launch.
Stuck on a broken v3.0.0 install? See `UPDATE_TROUBLESHOOTING.md`
— there may be a `.bak` you can rename.

### 🔍 Build verification fix
Builds that succeeded but were wrongly marked FAILED with a
"file count mismatch" (GTA III: The Definitive Edition was the
reported case) now pass correctly. The verify step now compares
**total bytes** — the exact, reliable signal — instead of file
counts that can differ by a couple of metadata entries on a
freshly-mounted exFAT volume. Genuine data loss is still caught.

### 🐛 Plus
- Queue row layout fix — long game titles no longer push the
  action buttons off-screen
- Tooltips on the Size pill
- Build log records which size override path was used

## 💬 Found a bug?

Open an issue with:
- Which game you were building / backporting
- The relevant `[INFO]` / `[VERIFY]` lines from OUTPUT LOG
- Your antivirus, if it's an update or launch issue

## 🙏 Credits

- BestPig for [BackPork](https://github.com/BestPig/BackPork)
  and [PS5-Backports](https://github.com/BestPig/PS5-Backports)
- SvenGDK for UFS2Tool
- NookieAI and stonemodder (Porkfolio) for the original
  workflow this tool automates
- PS5 Auto Backport pipeline (Backport.py, src/)	Nazky — github.com/Nazky
