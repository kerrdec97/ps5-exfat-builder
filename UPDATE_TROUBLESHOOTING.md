# Troubleshooting: "Failed to load Python DLL"

Some users see this error dialog after an auto-update:

```
Failed to load Python DLL
'C:\Users\...\AppData\Local\Temp\_MEI196362\python314.dll'.
LoadLibrary: The specified module could not be found.
```

This is a known PyInstaller issue (not specific to this app) that
happens when the auto-update flow races against antivirus or
leaves stale files behind. **v3.1.0 onwards** ships a hardened
auto-updater that catches most of the common causes — verifies
the download SHA, retains a backup of the working .exe, and
auto-rolls-back if the new version fails to launch.

If you're still seeing the error, here's what to do.

---

## 🚑 Quick recovery (if you're stuck right now)

The **v3.1.0+ auto-updater** backs up your previous working .exe
before overwriting it. So if you got the error after a v3.1.0+
update, look in the same folder as the .exe you launch:

1. Find `exFAT Image Builder.exe.bak`
2. Delete the broken `exFAT Image Builder.exe`
3. Rename `exFAT Image Builder.exe.bak` → `exFAT Image Builder.exe`
4. Double-click it. You're back to your previous working version.

> ⚠ **If there's no `.bak`** in the folder, you were updated by
> a v3.0.0 or older updater (which didn't create backups). Skip
> ahead to the **Clean install** section below — you'll need to
> download v3.1.0+ manually from GitHub.

Then either skip the next auto-update, or follow the **clean
install** steps below.

---

## 🧼 Clean install (the bulletproof fix)

If the rollback won't run either, or you just want a fresh start:

1. **Download the latest .exe manually** from
   <https://github.com/kerrdec97/ps5-exfat-builder/releases/latest>
2. **Whitelist the download folder in Windows Defender** before
   the download starts:
   - Open Windows Security → Virus & threat protection
   - "Manage settings" under Virus & threat protection settings
   - "Add or remove exclusions" → "Add an exclusion" → Folder
   - Pick the folder you'll save the .exe in
   This stops Defender from quarantining bits of the PyInstaller
   bundle as it extracts on first launch.
3. **Clean up old `_MEI` folders.** Press Win+R, type
   `%LOCALAPPDATA%\Temp` and Enter. Delete every folder whose
   name starts with `_MEI`. (Some may be locked; skip those.)
4. Double-click the freshly-downloaded .exe.

If it still won't launch after this, the cause is almost
certainly your antivirus blocking part of the bundle. Read on.

---

## 🛡 Antivirus & SmartScreen

PyInstaller-packed .exes get a bad reputation from antivirus
heuristics because malware authors also use PyInstaller. The
auto-extracting onefile format makes this worse: Defender sees a
new process writing DLLs to `%LOCALAPPDATA%\Temp\_MEI...\` and
can quarantine some of them mid-extraction, which produces the
exact "Failed to load Python DLL" symptom.

If you trust the build, add an **exclusion** for the .exe in
Windows Security. The repo also publishes a SHA256 next to each
release — you can verify it matches before adding the exclusion:

```powershell
Get-FileHash -Algorithm SHA256 "exFAT Image Builder.exe"
```

Compare the output against the `.sha256` file from the GitHub
release page. If they match, the binary is byte-identical to
what was published.

---

## 🪲 Reporting it

If none of the above helps, open an issue on GitHub with:

- Your Windows version (`winver`)
- Your antivirus (Defender, ESET, Kaspersky, Avast, etc.)
- The exact error message (a screenshot is fine)
- Whether `exFAT Image Builder.exe.bak` exists in the install
  folder (and whether the rollback worked)
- The contents of any log file under `%LOCALAPPDATA%\Temp\` from
  the failed launch attempt

<https://github.com/kerrdec97/ps5-exfat-builder/issues>

---

## ⚙ Why this happens (one-paragraph technical version)

PyInstaller onefile builds extract the Python runtime to
`%LOCALAPPDATA%\Temp\_MEI<random>\` at launch, then `LoadLibrary`
the bundled `python<ver>.dll`. The "specified module could not
be found" error is Windows reporting that **a dependency** of
that DLL is missing — not the DLL itself. The usual culprits are
antivirus quarantining `vcruntime140.dll` or `_ctypes.pyd` mid-
extraction, or the old `_MEI` folder from the previous version
still being mapped when the new process starts. The v3.1.0+
auto-updater waits for the old process to fully release its
file handles, verifies the download's SHA, keeps a backup, and
rolls back automatically if the new .exe fails to launch.
