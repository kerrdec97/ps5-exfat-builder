# Release checklist

How a release of exFAT Image Builder 5 is made. An agent may prepare
everything up to and including step 6 on a feature branch. Merging to
`main`, tags, GitHub releases, uploads and private builds are the owner's
(step 7).

## 1. Version and texts

* `native/CMakeLists.txt`: `project(FarewellEdition VERSION x.y.z ...)`. Every
  script reads the version from this line; `BuildInfo.h` (the app's
  version, `--self-test`) and the version resources of the app and the
  launcher take it from the project.
* `CHANGELOG.md`: a `## [x.y.z] - <date>` section at the top.
* `native/RELEASE_NOTES.md`: title line, the folder-zip name
  (`exFAT-Image-Builder-x.y.z-win64-folder.zip`) and what is new. The release
  workflow publishes this file as the release notes.
* `README.md`: the "Download x.y.z" line; bundled versions in the feature
  table and credits if a pin changed (pins change only with the owner).
* User-visible texts changed: `_source.json` regenerated (`FW_I18N_UPDATE=1`
  and `tst_i18n_source`), new texts listed for the translators.

## 2. Tests

All of CTest passes on Windows in a Release build (`make-release.ps1` runs it
again and stops on a failure). Console-only behaviour is listed as HARDWARE
REQUIRED in the release notes or the owner's test list, never claimed from
mock tests.

## 3. Build with `make-release.ps1`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native\packaging\make-release.ps1 `
    -QtDir '<Qt>\6.10.3\msvc2022_64' -Zip
```

| Option | Effect |
|---|---|
| `-QtDir <dir>` | Qt kit (default: `QT_ROOT_DIR`, `Qt6_DIR`, else `windeployqt` on `PATH`). Give it explicitly on this project |
| `-BuildDir <dir>` | Build folder (default `native\build-release`) |
| `-OutDir <dir>` | Output (default `dist\`, `dist-private\` for a private build) |
| `-BackportEngine <exe>` | A local copy of `PS5BackportGUI-0.3.4.exe`; empty: CMake downloads the pinned release. Checked against the SHA-256 in `cmake\BackportEngine.cmake` either way |
| `-Extra '<source>=<path inside>'` | Extra files in the one-file payload |
| `-Parallel N` | Build jobs |
| `-SkipBuild`, `-SkipTests` | Reuse a build / skip CTest (not for a release) |
| `-Zip` | Also the folder edition zip |
| `-PrivateToolkit`, `-PrivateDlcEngine`, `-PrivateYoutube` | **Private build only** (owner): bundles the owner's own Sony Publishing Tools, fpkg-cli or YouTube packages under `tools\`, output in `dist-private\`, licence and notices rewritten to say so. Never published, never committed |

What the script does: MSVC environment through `vswhere`/`VsDevCmd` if `cl`
is not on `PATH`; configure (Release, Ninja, `FW_WARNINGS_AS_ERRORS=OFF`) and
build; CTest with `QT_QPA_PLATFORM=offscreen`, `FW_OFFLINE=1`; deploy with
`windeployqt` plus README, LICENCE, LICENSES and notices (the
`<!-- public-edition -->` parts kept for a public build); checks that
`engines\backport\PS5BackportGUI.exe` and `engines\ampr-emu-ng\libSceAmpr.sprx`
match their pinned hashes; `finish-deploy.ps1` (VC++ runtime DLLs beside the
program, unused Qt plugins removed); a public build stops if
`Find-PrivateFiles` finds private tools; `make-onefile.ps1` packs and
verifies the one-file exe; smoke test; zip; `SHA256SUMS.txt`.

Outputs in `dist\`: `exFAT Image Builder.exe` (the download), the deploy
folder `exFAT Image Builder\`, with `-Zip`
`exFAT-Image-Builder-x.y.z-win64-folder.zip`, and `SHA256SUMS.txt`.

## 4. Checksums

`SHA256SUMS.txt` lists every asset with LF line ends, the exe under the name
GitHub serves it (`exFAT.Image.Builder.exe`: spaces become dots). Check it:

```powershell
Get-FileHash "dist\exFAT Image Builder.exe" -Algorithm SHA256
Get-FileHash "dist\exFAT-Image-Builder-x.y.z-win64-folder.zip" -Algorithm SHA256
Get-Content dist\SHA256SUMS.txt
```

## 5. Defender scan

Scan the outputs with the PC's own Microsoft Defender (no exclusions, no
setting changes):

```powershell
& "$env:ProgramFiles\Windows Defender\MpCmdRun.exe" -Scan -ScanType 3 -File "<repo>\dist" -DisableRemediation
```

Record the result. The exe is not code-signed and unpacks itself, so some
engines flag it by heuristics (`README.md`, "Antivirus warnings"). Whether to
upload it to VirusTotal is the owner's choice.

## 6. Smoke test

`make-release.ps1` already runs `--self-test` three times from a fresh temp
folder (first start of the one-file exe, cached start, folder edition) with
private `LOCALAPPDATA`, `FW_DATA_DIR`, `D97BETA_*` folders, `FW_OFFLINE=1`
and `FW_NO_POWER_ACTIONS=1`, and checks the reported version. By hand, with
the same isolation variables set:

* `"dist\exFAT Image Builder\exFAT Image Builder.exe" --screenshot-pages <tmp>\pages`
  and look at the pages;
* start the one-file exe, check that it stays up, open every page, close it;
* `fw_onefile_pack.exe list "dist\exFAT Image Builder.exe"` (in
  `<build>\bin`) shows the payload: no `tools\fpkg-toolkit`, `tools\fpkg-cli`
  or `tools\youtube`, no `.pkg`.

Real game, real Sony tools and console checks are the owner's (HARDWARE
REQUIRED).

## 7. What the owner does

* Reviews the branch and merges it into `main`.
* Tags `vx.y.z` and pushes the tag. `native-release.yml` then builds on the
  runner and creates a **pre-release** with the exe and `SHA256SUMS.txt`
  unless a release for the tag already exists. The owner normally publishes
  the release by hand with the files from step 3 (exe, folder zip,
  `SHA256SUMS.txt`), and promotes a pre-release after testing on a real PC
  and console.
* Makes a private build if wanted (`-Private...` switches) and keeps it
  private.
* Answers and closes GitHub issues.

## Never

* Private build files (`dist-private\`, `tools\fpkg-toolkit`,
  `tools\fpkg-cli`, `tools\youtube`, Sony DLLs, `.pkg` files) in the
  repository, a public release or a CI artefact.
* A release from a build with failing tests, or with engine files other than
  the pinned ones.
* Tags, releases or pushes to `main` by an agent.
