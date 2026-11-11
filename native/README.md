# exFAT Image Builder 5 (native C++)

exFAT Image Builder 5 is a native C++20 / Qt 6 Windows application for PS5
game dumps. It builds, verifies, extracts, converts and edits PS5 game images
(exFAT, FFPKG/UFS2, FFPFSC and FFPFS/PFS), builds FPKG packages with your own
Sony Publishing Tools, backports games to older firmware with the bundled PS5
Backport engine, and talks to a PS5 over the network (FTP, payloads, kernel
log, ShadowMount+). It replaces the Python application of v1–v4.

> For games and content you legally own and dumped yourself. It does not
> download games, decrypt retail packages or bypass DRM, and it is not
> affiliated with or endorsed by Sony Interactive Entertainment.

## Requirements

* Windows 10 1809 or later, 64-bit (Windows 11 recommended).
* No administrator rights, OSFMount, .NET or Python: image formats are written
  and read natively.
* Optional, user-supplied: Sony Publishing Tools (`prospero-pub-cmd.exe`) for
  FPKG packages. Nothing from Sony is included or downloaded.

## Install

* **Single file (recommended)**: download `exFAT Image Builder.exe` and run
  it; nothing to unzip or install, no admin prompt, no VC++ Redistributable
  needed. The first start of each version unpacks the program once (about
  190 MB, a few seconds) to
  `%LOCALAPPDATA%\kerrdec97\exFAT Image Builder\runtime\` (or `%TEMP%` if that
  is not writable); later starts run it from there at once, and older versions
  are removed. A damaged download refuses to start. With an empty
  `portable.txt` next to the exe, the unpacked copy goes to
  `exFAT Image Builder.data\` and settings and logs to `data\` beside it.
  To remove the app, delete the exe and that `runtime` folder.
* **Folder edition**: `exFAT-Image-Builder-<version>-win64-folder.zip` is the
  same program already unpacked. Extract it anywhere (an external drive
  works) and run the exe inside; nothing is unpacked to your profile. Use it
  when an antivirus removes the single exe. `portable.txt` next to that exe
  keeps settings and logs in `data\` beside it.
* Verify the download with `SHA256SUMS.txt` from the same release.

## Where things are stored

| What | Default location | Portable mode |
|---|---|---|
| Settings, queue, history | `%APPDATA%\kerrdec97\exFAT Image Builder Farewell` | `<app>\data` |
| Logs (text + JSON lines), crash reports | `%LOCALAPPDATA%\kerrdec97\exFAT Image Builder Farewell\logs` (or Settings › Logging › Log folder) | `<app>\data\logs` |
| Temporary files | `%LOCALAPPDATA%\...\temp` (or Settings › Storage › Temporary folder); folders older than a day are removed at start | `<app>\data\temp` |

Settings from the legacy app (`~/.exfat_builder_settings.json`) can be
imported from Settings › Advanced.

## Command line

```
"exFAT Image Builder.exe" [--self-test] [--screenshot-pages <dir>] [--quit-after <ms>]
```

* `--self-test` prints a JSON report of every dependency and the registered
  settings and job types, then exits. The single-file exe passes the command
  line, working folder and exit code through, and shows the output in the
  console it was started from (or wherever it is redirected).
* `--screenshot-pages <dir>` renders every page to PNG files.

## Licence

Proprietary: Copyright © 2026 Deckerr97 (kerrdec97), all rights reserved
(`packaging/LICENCE.txt`). Third-party components keep their own licences:
the PFS engine is a port of Lazy_MkPFS / PSBrew MkPFS (GPL-3.0), the UFS2
engine follows UFS2Tool's layout (BSD-2-Clause), Qt is used under the
LGPL-3.0, icons are Lucide (ISC). See `packaging/THIRD_PARTY_NOTICES-native.md`
and the `LICENSES` folder.
