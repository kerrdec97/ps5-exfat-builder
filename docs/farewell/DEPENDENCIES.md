# Dependency & toolchain inventory — exFAT Image Builder 4.1.0 (Farewell Edition)

| | |
|---|---|
| Snapshot | branch `farewell-edition-dev`, HEAD `db14125` (2026-11-03). The working tree had uncommitted edits; all embedded blobs are byte-identical between HEAD and the working tree. |
| Method | Static analysis only: AST import scan of every `.py` file, base64 blobs decoded, PE version resources parsed, wheels downloaded with `pip download --no-deps` (not installed), upstream repos read with `git ls-remote` / blobless clones, PyPI JSON (`/pypi/<pkg>/<ver>/json`, which carries OSV advisory data). Nothing was built or run on Windows. |
| Not reachable | GitHub REST/GraphQL for third-party repos, `github.com` HTML pages, api.osv.dev, endoflife.date, python.org. See "Could not verify" at the end. |

## Headline

- **8 pinned Python packages.** All 8 are used. 2 are already the latest release (psutil, zlib-ng).
- **Pillow 12.3.0 has 39 published advisories, all fixed in 13.0.0.** They include 2 "Critical" and 12 "High" ones in Pillow's own release notes. The app runs `Image.open()` on cover art from user-supplied game folders, so the vulnerable decoders can be reached. **Update.**
- **PyInstaller 6.16.0 → 6.22.3.** 6.22.1 added bootloader hardening against `GHSA-9fxf-4qw3-ghmr` (environment spoofing of elevated onefile executables). This app is onefile and relaunches itself as administrator. **Update.**
- **.NET 8 reaches end of support on 2026-11-10, one week from now.** This is verified from `dotnet/core` release metadata. The embedded UFS2Tool targets `net8.0`, and the app-written `runtimeconfig.json` has no `rollForward`, so UFS2Tool will not start on a PC that only has .NET 9 or 10.
- **12 embedded base64 blobs**: 3 PE binaries (UFS2Tool.exe/.dll 4.0.0, DokanNet.dll 2.0.1), 6 scripts and 3 assets. **DokanNet.dll is byte-identical to the official NuGet `DokanNet 2.0.1` package** (verified). DokanNet (MIT) is **missing from THIRD_PARTY_NOTICES.md / LICENSES/**.
- **Vendored Lazy_MkPFS is current.** `78bdd00` is still the HEAD of upstream's default branch (`lazy`), and only the two documented vendoring patches differ. Upstream's `lazy` branch has **no LICENSE file** (it was deleted in `60a1722`). The GPL-3.0 text comes from the repo's `main` branch, a mirror of PSBrew/MkPFS.
- Unpinned but imported: `tkinterweb` and `capstone` are optional, with a graceful fallback, and are not bundled. `requests` is not used anywhere: networking is stdlib `urllib` / `http.client`.

---

## 1. Python packages

### 1a. Pinned in `requirements-build.txt`

Latest versions are from PyPI on 2026-11-03. Every latest version has a cp311 `win_amd64` (or abi3 / py3-none) wheel.

| Package | Pinned | Latest | Licence | Used where (verified) | Advisories on the pinned version | Rec. |
|---|---|---|---|---|---|---|
| pyinstaller | 6.16.0 (2025-09-13) | **6.22.3** (2026-09-12) | GPL-2.0-or-later with bootloader exception | Build only: `build.bat` | No PyPI/OSV record. The PyInstaller 6.22.1 changelog references **GHSA-9fxf-4qw3-ghmr** (onefile inherited-environment validation). Severity and affected range are unverified. | **Update** |
| pillow | 12.3.0 (2026-07-01) | **13.0.0** (2026-10-16) | MIT-CMU (HPND) | 26 import sites: cover art (`exfat_builder.py` `_normalize_cover` L657-L716, L7297, L7586), icons (`ui/icons.py`), `tkinter_theme.py`, `ui/shared/widgets.py`, `ui/shared/hero.py`, ImageTk in about 10 tabs, `tools/gui_check.py` (ImageGrab, CI only) | **39 unique advisories** (PyPI/OSV), all `fixed_in 13.0.0`. Critical: CVE-2026-96639 / GHSA-v9pv-5gc8-w2m6; CVE-2026-112398 + CVE-2026-112403 / GHSA-5x8w-8jj5-q52h + GHSA-8g5g-8vmr-9782 (JPEG/FlashPix heap overflow). High (examples): GHSA-9v9v-9qvf-h2hj (WMF/EMF on Windows), GHSA-g7cr-3p24-7wf7, GHSA-462x-pgg6-55fp, GHSA-c6hw-5grf-ff96, GHSA-78g4-xqwv-q3g8 (bundled OpenJPEG). The full list is in Pillow's `docs/releasenotes/13.0.0.rst`. | **Update** |
| psutil | 7.2.2 | 7.2.2 | BSD-3-Clause | `exfat_builder.py` L3036 (`virtual_memory`, diagnostics), L24530 (`Process`, `cpu_percent`), `ui/tab_unibuild.py` L1460 (`disk_io_counters`) | none | Keep |
| tkinterdnd2 | 0.6.2 (2026-07-05) | 0.7.0 (2026-10-25) | MIT (wrapper, © Philippe Gagné); bundles the tkdnd 2.10.x binaries | `exfat_builder.py` L26: optional drag and drop, falls back to `tk.Tk` | none | Update (optional) |
| mkpfs | 0.0.9 (2026-06-27) | **1.1.0** (2026-10-03) | GPL-3.0 (PSBrew/MkPFS) | **The primary PFS engine.** `ui/mkpfs_runner.py` (`from mkpfs.__main__ import main`, `mkpfs.exfat`), the worker shim at `exfat_builder.py` ~L26659, argv built in `exfat_builder.py` L22766/L23796, `ui/tab_pfs.py` L953/L999, `ui/tab_extract.py` L1372-L1528, `ui/tab_edit.py` L423 | none | **Defer** (see R7) |
| cryptography | 49.0.0 (2026-06-12) | **50.0.2** (2026-09-30) | Apache-2.0 OR BSD-3-Clause; the wheel bundles OpenSSL (Apache-2.0) | `vendor/lazy_mkpfs/lazy_mkpfs/crypto.py` (AES via `Cipher/algorithms/modes`). mkpfs 0.0.9 requires `cryptography>=47`. | **GHSA-g6cj-pr64-35w5 / CVE-2026-69247 / PYSEC-2026-3552**, fixed in 50.0.0: a Bleichenbacher-style oracle in `pkcs7_decrypt_*`. **Not reachable**, because the app never uses PKCS#7. 50.0.1 and 50.0.2 also rebuild against OpenSSL 4.0.2 and 4.0.3. | **Update** |
| zlib-ng | 1.0.0 (2025-09-10) | 1.0.0 | PSF-2.0 (python-zlib-ng); bundles zlib-ng (Zlib licence) | **Required, not optional**: mkpfs 0.0.9 `pfs.py:33` does `from zlib_ng import zlib_ng as zlib` unconditionally. Lazy's `--zlib-ng` path is never used by the app. | none | Keep; fix the comment in requirements |
| isal | 1.8.0 (2025-09-10) | 1.9.1 (2026-11-03, published today) | PSF-2.0 (python-isal); bundles ISA-L (BSD-3-Clause) | Opt-in only: `vendor/lazy_mkpfs/lazy_mkpfs/compression.py:48`, reached when the user picks the "isa-l" backend (`ui/experimental_mkpfs.py` L185-L192) | none | Keep (wait) |

### 1b. Unpinned transitive build dependencies

`pyinstaller-hooks-contrib` (latest 2026.9), `altgraph`, `pefile`, `packaging`, `setuptools`, `pywin32-ctypes`, `cffi` (latest 2.1.1), `pycparser`. `build.bat` also runs `pip install --upgrade pip` without a pinned version.

Effect: two builds of the same commit can bundle different hook or cffi versions. `pyinstaller-hooks-contrib` affects what ends up in the exe.

### 1c. Imported but not pinned

| Module | Where | Behaviour when absent | Latest / licence | Rec. |
|---|---|---|---|---|
| tkinterweb | `ui/tab_help.py:72` | `except ImportError` → built-in help renderer | 4.25.5, MIT (+ native Tkhtml) | Keep unbundled. It would add native Tkhtml DLLs, which means more antivirus surface. |
| capstone | `ui/audio_channel_scan.py:255` | Returns `None`, so there is no disassembly listing | 5.0.9, BSD | Keep unbundled |
| customtkinter | not imported by the app | — | Becomes a **hard dependency with mkpfs ≥1.1.0** (`requires_dist`) | See R7 |
| requests | **not used** | — | — | — |
| Backport / src (`Archive`, `make_fself`, …) | embedded sources, see §2 | — | — | — |

No other third-party imports exist across `exfat_builder.py`, `tkinter_theme.py`, `ui/`, `vendor/lazy_mkpfs`, `tools/` or `tests/`. The embedded Backport sources import only the stdlib and their own `src` package. They call the external `7z` / `unrar` tools when those are present (see §4).

---

## 2. Embedded binaries and scripts (`exfat_builder.py`)

SHA-256 values are of the **decoded** bytes. They can be reproduced with `scratchpad/agents/deps/scripts/extract.py`. Line numbers refer to HEAD `db14125`.

| Constant (line) | Decoded as | Bytes | SHA-256 | Version / identity | Upstream | Licence | Latest upstream |
|---|---|---|---|---|---|---|---|
| `_UFS2TOOL_EXE_B64` (L88) | UFS2Tool.exe: .NET apphost, PE32+ x64, unsigned | 152,064 | `049105934ae1f92ba9b738dc4972f7a8ed07aefffda6832a12b0b0dc2d71ff61` | FileVersion 4.0.0.0, ProductVersion `4.0.0+c123639cdd2d…` | SvenGDK/UFS2Tool | BSD-2-Clause (verified upstream) | **v4.1** (`00317e3`, 2026-05-08) plus 3 later commits on HEAD `b5307a6` |
| `_UFS2TOOL_DLL_B64` (L89) | UFS2Tool.dll: managed assembly, `.NETCoreApp,Version=v8.0`, unsigned | 211,968 | `08f2f2a17f8a1d7cd57473233143c91dc1d44d49675e41bd81d34e27b8828e3a` | 4.0.0.0, built from commit `c123639` (2026-02-13) | same | same | same. Upstream HEAD still targets `net8.0`. |
| `_DOKAN_B64` (L90) | DokanNet.dll: netstandard2.0, PE32 AnyCPU, unsigned | 72,704 | `129ae261f6bfd9cd5c3d4ae77ad44e81a3f2169f40a4016279b618ec86089387` | 2.0.1.0. **Identical to `lib/netstandard2.0/DokanNet.dll` in NuGet `dokannet.2.0.1.nupkg`** | dokan-dev/dokan-dotnet | **MIT** (nuspec + `license.txt`) — **missing from the notices** | NuGet 2.3.0.3. Needs the Dokany 2.x driver (latest tag v2.3.1.1000). |
| `_PS1_B64` (L86) | New-OsfExfatImage.ps1 | 44,920 | `0c0545c3c8aa447a5be823ba7d1f436bf3193b9a3c34949ccfbef6d069a7582e` | "v2 — large-game-friendly" | Project's own | Project's own | — |
| `_BAT_B64` (L80) | make_image.bat | 4,630 | `2be25e451e5c1d356e6f2ebe772830bff4c97c0947688a7afe076f111d6ab188` | "v5" | Project's own | Project's own | — |
| `_BACKPORT_B64` (L81) | Backport.py | 112,508 | `e88be878673c7a24c90f0840d72567b0f46c884674cf0a9a4bb8e44775f042a1` | No version string | Nazky Auto-Backport. No repo URL in the code. | Inherits its components' licences (per the notices) | Not determinable |
| `_SRC_ARCHIVE_B64` (L82) | src/Archive.py | 26,910 | `604145517034a18ab8e90647388082c454a3e7c6391c838459f6e97ed246ae96` | — | Nazky | as above | Not determinable |
| `_SRC_FSELF_B64` (L84) | src/make_fself.py | 32,943 | `5d4397bc71133317e1ad42a9d9f19c1cc8b0907a28c60dfdba6ba33556ae0e6c` | Nazky-modified (upstream is 26,685 bytes) | ps5-payload-dev/sdk `samples/install_app/make_fself.py` | GPL-3.0 (verified, sdk `LICENSE`) | Upstream file last changed `88dd7b0` (2025-12-20) |
| `_SRC_DECRYPT_B64` (L83) | src/decrypt_fself.py | 25,229 | `07bebda29213bd5ae841b3d2d56b6a6264ed3ccd32aeeb2baf465dd1d093ca6d` | Based on make_fself, PS4/PS5 SELF magic added | as above | GPL-3.0 | — |
| `_SRC_PATCHER_B64` (L85) | src/ps5_sdk_version_patcher.py | 17,321 | `45b04cfc9157bff39026512b30d0bee2d6f8e0749cb610ab8e629b1f3f556727` | Nazky-modified | gist idlesauce/2ded24b7… (`ps5_elf_sdk_downgrade.py`, last rev 2025-11-04) | **No licence**: the gist was cloned and contains no licence text. [VERIFY] stays open. | — |
| `_TUTORIAL_HTML_B64` (L78) | Help tutorial HTML | 15,087 | `aef96c851ba2f63c2b4613db3d4579d5076527e380426c56521fb2ba3309db91` | Says **"v1.7.1"** (stale; the app is 4.1.0) | Project's own | — | — |
| `_ICO_B64` (L92) | icon.ico (16×16 PNG-in-ICO) | 753 | `f75b3d2bdbd6884bd8df579468b702866497591fd296a7a2388568a3d93c0fca` | Identical to `controller.ico` | Project's own | — | — |

Notes:
- **UFS2Tool version.** The binary says 4.0.0 but was built from `c123639`. That commit's csproj still said `3.0.0`, and it is two days before tag `v4.0` (`f9c4911`, an ancestor). So this is a near-v4.0 developer build, not necessarily the official v4.0 release asset, which I could not fetch (GitHub API blocked).
- **What v4.1 changes** (commit `00317e3`): DokanNet 2.0.1 → 2.3.0.3, `fsck_ufs -b` fix, `newfs -S 4096` fix. The app always passes `-S 512` (`exfat_builder.py` L7901, `ui/tab_ffpkg_edit.py` L2384), so the `-S 4096` fix does not affect current use.
- **Extraction.** `extract_ufs2tool()` (L751) writes the three PE files plus a generated `UFS2Tool.runtimeconfig.json` (`tfm net8.0`, framework `Microsoft.NETCore.App 8.0.0`, **no `rollForward`**) to a fresh `tempfile.mkdtemp()`. `extract_scripts()` (L522) does the same for the .bat, .ps1 and .ico. `extract_backport()` (L705) writes Backport.py and `src/`. No hash is checked between write and exec.

---

## 3. Vendored code: `vendor/lazy_mkpfs`

| Item | Finding |
|---|---|
| Vendored commit | `78bdd00c5645b1170420df051c586dd0ca4f918d` (2026-06-18, "oups") |
| Upstream default branch `lazy` | **HEAD = `78bdd00`, so there are no upstream changes since vendoring.** |
| Upstream `main` branch | `3fd1f1f` (2026-06-23). This is a PSBrew/MkPFS mirror, not Lazy code. It has 11 commits not in `lazy` (AMPR emu index, OS-metadata exclusion, zlib-ng backend, faster unpack, ruff/gitleaks), all by PSBrew contributors. |
| Local diff vs upstream | Only `lazy_mkpfs/__init__.py` (10 lines: `_ensure_dependencies()` pip auto-install disabled) and `lazy_mkpfs/compression.py` (47 lines: `_auto_install` turned into a no-op). The other 18 files are byte-identical. `LICENSE`, `UPSTREAM_COMMIT.txt` and `UPSTREAM_README.md` were added locally. |
| Licence | The `lazy` branch has **no LICENSE file**: commit `60a1722` "i am lazy" deleted the 674-line GPL-3.0 LICENSE. The vendored `LICENSE` is byte-identical (sha256 `3972dc97…6986`) to the LICENSE on upstream `main` and to `LICENSES/GPL-3.0.txt`. THIRD_PARTY_NOTICES.md says "upstream `LICENSE` is GPL-3.0". That is true of the repo, but not of the vendored branch. Reword it as "GPL-3.0 inherited from PSBrew/MkPFS; the LICENSE was removed on the `lazy` branch". |
| Related upstream | PSBrew/MkPFS (the base of Lazy, and the PyPI `mkpfs`) has moved on to 1.1.0 (`1d5df56`, 2026-10-03) with 136 non-merge commits after 0.0.9. Notable: `8503082` *"Prevent PS5-unsafe automatic ISA-L compression (#135)"*. Upstream says ISA-L streams **can trigger PS5 hardware inflater errors (issue #132)**. That is relevant to the app's opt-in "isa-l" Lazy backend. |
| Docstring drift | `ui/experimental_mkpfs.py` says "never pass --zlib-ng / --zlib-isa", but `_build_worker_cmd` passes `--zlib-isa` when the user picks ISA-L. |

---

## 4. External runtime tools (called, not shipped)

| Tool | Used for | How it is found / version handling | Notes |
|---|---|---|---|
| **OSFMount** (PassMark) | Mount, format and dismount raw and exFAT images: `osfmount.com -a -t file -f <img> -m <X:> -o rw,rem` / `ro,rem`, and `-d -m X:` (`exfat_builder.py` L8654, L11147, L11322, L22977; `ui/tab_files.py`; `ui/tab_ffpkg_edit.py`; inside the .ps1) | `_find_osfmount()` (L8165) searches the settings path, then `HKLM\SOFTWARE\PassMark\OSFMount`, then Program Files / x86 / PassMark, then PATH. It prefers `osfmount.com` over the GUI `OSFMount.exe`, and it distrusts a lone `osfmount.exe` because that may be the installer (#86). The version is only *displayed*, via `GetFileVersionInfoW` (`ui/tab_settings.py:366`). **No minimum version is enforced.** Comments assume v3+. | User installs it from osforensics.com. Timeouts were added in #71. |
| **robocopy** | Copy into the mounted image (.ps1: `/MT:N` adaptive 8/32, `/J` for files ≥1 GB, `/R:2 /W:2 /TBD`, `/COMPRESS` for network sources only) | Assumed present (Windows built-in) | `/COMPRESS` is only in newer robocopy builds. It is gated to UNC sources only, but is untested on Windows 10 network sources (unverified). |
| **PowerShell 5.1** (`powershell.exe`) | Runs the embedded .ps1 (`make_image.bat` L78: `-NoProfile -ExecutionPolicy Bypass -File`). Also `Get-PhysicalDisk` (`ui/tab_advanced.py:83`) and the SHA-256 step in `build.bat`. | Assumed present. No `#Requires`. | `pwsh` is used only in CI. |
| **format.com** | exFAT format with retry strategies (.ps1 L565-L599; `ui/tab_convert.py` L1170) | Windows built-in | — |
| **mountvol**, **attrib**, **cmd.exe**, **explorer** | Dismount fallback (`exfat_builder.py` L9195), attributes, running the .bat, opening folders | Windows built-in | — |
| **wmic** | Disk media-type fallback (`ui/tab_advanced.py:129`) | Called only if `Get-PhysicalDisk` fails | WMIC is deprecated and absent by default on recent Windows 11, so this fallback quietly fails there (from knowledge). |
| **.NET 8 Runtime (x64)** | UFS2Tool (`newfs`, `mount_udf`, `umount_udf`, `extract`) | **Reactive only**: `dotnet_runtime_hint()` (L2640) maps apphost errors 0x80008081/83/96 to a "install .NET 8" message (#70). No pre-check. | **EOL 2026-11-10** (verified: `dotnet/core` `release-notes/8.0/releases.json` `eol-date`, latest 8.0.33). .NET 9 ends on the same date. .NET 10 LTS runs to 2028-11-14. |
| **Dokany driver v2** (`dokan2.sys` / `dokan2.dll`) | Needed by UFS2Tool `mount_udf` (mount-based .ffpkg extract) | `_find_dokan_driver()` (L4812) checks `System32\drivers\dokan*.sys` and `System32\dokan2.dll`. The README says v1 is not compatible. | DokanNet 2.0.1 binds `dokan2.dll`. Latest Dokany is v2.3.1.1000. |
| **7-Zip `7z`, `unrar`/`rar`, WinRAR** | Backport archive extraction (embedded Archive.py and Backport.py). Backport.py adds `C:\Program Files\WinRAR` / 7-Zip to PATH. | Probed with `subprocess.run([tool])` | Optional. Zip and tar go through stdlib with per-member path-traversal checks; `extractall` is not used. |
| PS5 console services | FTP, payload sender (`http.client`, port configurable), klog | — | Not a local dependency |

---

## 5. Build toolchain

| Item | Current | Finding |
|---|---|---|
| Python | 3.11 x64, asserted by `build.bat` (`py -3.11`, `sys.version_info[:2]==(3,11)`). CI (`.github/workflows/windows-build.yml`, added in `d77448a`) uses `actions/setup-python@v5` 3.11. | 3.11 is security-fix-only (latest tag `v3.11.18`, verified with ls-remote). From knowledge, **python.org publishes no Windows installers for security-only releases, so a hand-built release uses ≤3.11.9 (April 2024).** CI builds get a newer 3.11.x from `actions/python-versions`, so release builds should come from CI. Pillow 13, cryptography 50, isal 1.9.1, zlib-ng 1.0.0 and psutil all have cp313/cp314 win_amd64 wheels, so moving up is possible. |
| PyInstaller flags | `--onefile --windowed --clean --noconfirm --noupx --version-file … --add-data vendor/lazy_mkpfs,CHANGELOG,THIRD_PARTY_NOTICES,LICENSES,controller.ico`, many `--hidden-import`s, `--collect-all mkpfs/cryptography/tkinterdnd2`, `--collect-submodules lazy_mkpfs/ui/ui.shared`, `--exclude-module numpy/yaml/charset_normalizer/pytest` | `--noupx` is good. `--collect-all tkinterdnd2` ships the tkdnd binaries for **all 12 platform folders** (Linux .so, macOS .dylib, arm64, x86, Tcl9), about 2.7 MB of unneeded native code. The `cryptography.hazmat.backends.openssl.backend` hidden import still exists in 49.0.0 and 50.0.2 (checked in the wheels). A Lite build is referenced, but `exfat_builder_lite.py` does not exist, so that step is skipped. |
| Reproducibility | Pins cover top-level packages only, with no hashes. `pip` itself is unpinned. GitHub Actions are pinned by tag (`@v4` / `@v5`), not by SHA. | See R10. |
| Elevation | Runtime `ShellExecuteW(... 'runas' ...)` self-relaunch (L815), not a manifest | — |

### Antivirus false positives (#63, #78: `Trojan:Win32/Wacatac.B!ml`)

These factors are present in the build:
1. **Onefile self-extraction** to `%TEMP%\_MEIxxxx` using the stock, unsigned PyInstaller bootloader, which is shared with a lot of malware.
2. **Dropper pattern.** Three PE files stored as base64 inside the PYZ are written to `%TEMP%\exfat_builder_ufs2_*` and executed. A `.bat` and a `.ps1` are dropped and run with `-ExecutionPolicy Bypass`.
3. **Elevated self-relaunch** plus system tools: `format.com`, `mountvol`, OSFMount driver calls, `attrib`, `wmic`.
4. **Network and self-update.** The app downloads a new exe from GitHub releases and replaces itself. It also does FTP and HTTP to LAN hosts.
5. **No Authenticode signature** on the app or on any embedded PE (all three have an empty security directory).
6. UPX is **not** used (`--noupx`), and version info is embedded (`tools/make_version_info.py`). Both are already done.

Concrete mitigations, roughly in order of effect:
1. **Code-sign** the exe. Options: SignPath Foundation (free for OSS), Azure Trusted Signing, or a Certum open-source certificate. Signing is the only reliable fix, and it also makes the updater checkable (Authenticode before swapping in a new exe).
2. **Ship onedir as a zip** instead of onefile. That removes the self-extracting stub. Optionally offer both.
3. **Stop embedding PE files as base64.** Ship UFS2Tool.exe/.dll and DokanNet.dll with `--add-binary`, or as loose files in a onedir build. Ship the .ps1/.bat with `--add-data`. Verify a pinned SHA-256 right before execution. This also shrinks `exfat_builder.py` by about 2 MB and makes the code reviewable.
4. **Rebuild the PyInstaller bootloader from source** in CI (`PYINSTALLER_COMPILE_BOOTLOADER=1 pip install pyinstaller==6.22.3 --no-binary pyinstaller`). That avoids hash and ML matches against the stock bootloader.
5. Drop `--collect-all tkinterdnd2` (see R6) so no foreign-platform binaries end up inside a Windows PE.
6. Submit every release to the Microsoft WDSI false-positive portal and keep publishing the SHA-256. The README already explains the issue.

---

## 6. Icons: `ui/icons_data.py`

148 entries of 96×96 8-bit alpha PNG masks (base64), rendered from **Lucide 0.460.0** plus one original "eject" glyph. Licence: **ISC** (Lucide), with portions MIT (Feather © Cole Bemis); the full text is in `LICENSES/Lucide-ISC.txt` (verified). Latest Lucide is 1.67.0 on npm (ISC). The masks are static, carry no code and pose no security risk. **Keep.** Re-render only if a new glyph is needed.

---

## 7. Licence and notice gaps found

| Gap | Fix |
|---|---|
| **DokanNet 2.0.1 (MIT)** is embedded and redistributed but not listed in the notices or `LICENSES/`. | Add an entry and `LICENSES/DokanNet-MIT.txt` (text from dokan-dev/dokan-dotnet `license.txt`). |
| Bundled PyPI packages are not listed at all: **mkpfs (GPL-3.0)**, Pillow (MIT-CMU, plus bundled libjpeg-turbo, libpng, libtiff, freetype, OpenJPEG, libwebp and others), cryptography (Apache-2.0/BSD, plus OpenSSL Apache-2.0), psutil (BSD-3), tkinterdnd2 (MIT) and tkdnd, python-zlib-ng / python-isal (PSF-2.0, plus zlib-ng Zlib and ISA-L BSD-3), the PyInstaller bootloader (GPL-2.0+ with exception), CPython (PSF-2.0) and Tcl/Tk. | Add a "Bundled Python runtime and packages" section. `pip-licenses --from=mixed --with-license-file` run in `.build-venv` can generate it. |
| The Lazy_MkPFS "verified upstream LICENSE" wording | See §3 |
| idlesauce gist has no licence | Confirmed: the gist contains no licence. Keep [VERIFY] or ask the author. |
| No top-level `LICENSE` in the repo | THIRD_PARTY_NOTICES.md itself recommends GPL-3.0. That decision belongs to the owner. |

---

## Recommended updates

Each item gives the justification, the risk and how to test it. "Test baseline" means: `tools/release_check.py`, `python -m unittest discover -s tests`, `EXFAT_GUI_SMOKE=1` smoke test and `tools/gui_check.py` in en/de/ja. The CI workflow already runs all of these.

### R1. Pillow 12.3.0 → 13.0.0 — UPDATE (security)
- **Why:** 39 advisories, fixed only in 13.0.0 (no 12.x backport release exists). The vulnerable decoders can be reached: `_normalize_cover()` and the 96×96 cover loads call `Image.open(path)` on files from user-supplied game dumps. Pillow detects the format from the content, so a file named `icon0.png` can reach the TIFF, MSP, BMP-RLE, WMF or FlashPix decoders.
- **Risk: low.** The app uses `Image.open`, `.resize(…, Image.LANCZOS)`, `.save(…, 'PNG')`, `ImageDraw` and `ImageTk`. None of these are touched by 13.0.0's breaking changes: dropped Python 3.10, I-mode PNG clipping, `fromarray(mode)`, `get_child_images`, `_show`, `ImageCms.product_*`, `ExifTags.IFD.Makernote`, and empty small-ICO saves. `Image.LANCZOS` still exists.
- **Also:** add `formats=("PNG", "JPEG", "WEBP")` (or just PNG and JPEG) to the cover-art `Image.open` calls at `exfat_builder.py` L675, L7301 and L7587. That rules out the exotic decoders whatever the Pillow version.
- **Test:** the test baseline. Then open the Library, PFS and Backports tabs with real covers (PNG and JPEG). Check the cover cache in `~/exfat_builder_logs/covers_norm` and the icons at 100%, 150% and 200% DPI.

### R2. PyInstaller 6.16.0 → 6.22.3, and pin `pyinstaller-hooks-contrib==2026.9` — UPDATE (security hardening)
- **Why:** 6.22.1 adds parent-process and environment validation for onefile children (GHSA-9fxf-4qw3-ghmr), which matters for UAC-elevated onefile apps. 6.22.3 limits that validation to elevated processes and fixes nested sub-processes (#9513). This app is elevated, onefile, and re-execs itself (`--__mkpfs_worker__`, `--__lazy_worker__`, plus multiprocessing pools inside mkpfs and Lazy).
- **Risk: medium.** The new validation applies to exactly this app's pattern of elevated, nested `sys.executable` workers. 6.22.0–6.22.2 had regressions that were fixed in 6.22.3, so do not stop at an intermediate version.
- **Test:** build in CI. Run the exe **elevated**. Do a stock-mkpfs folder pack (multi-core), a Lazy ffpfsc convert (multi-core, then `--cpu-count 1`), verify, deep unpack, and a UFS2Tool newfs. Confirm there is no "security validation" error, and that `_MEI*` folders are cleaned up after exit.

### R3. cryptography 49.0.0 → 50.0.2 — UPDATE
- **Why:** it clears CVE-2026-69247 from scanners (not reachable here) and picks up OpenSSL 4.0.3 builds.
- **Risk: low.** The app uses only AES `Cipher` / `algorithms` / `modes`. 50.0.0 deprecates FFDH and tightens X.509/DER parsing, none of which the app uses. All `--hidden-import` paths still exist in the 50.0.2 wheel. mkpfs 0.0.9 accepts it (`>=47`).
- **Test:** the test baseline, an encrypted or signed PFS round trip if the UI offers one, and `verify` on a known-good image. Compare a PFS built with 49 and with 50 byte for byte; it should be identical.

### R4. UFS2Tool runtime: add `rollForward` before 2026-11-10 — CHANGE (compatibility)
- **Why:** .NET 8 and 9 reach end of support on 2026-11-10 (verified). New PCs will increasingly have only .NET 10, and today UFS2Tool refuses to start without an 8.x runtime.
- **What:** in `extract_ufs2tool()` (L762), add `"rollForward":"LatestMajor"` (or `"Major"`) to `runtimeOptions`. Update the `dotnet_runtime_hint` text to say ".NET 8 or later".
- **Risk: low–medium.** .NET is highly backward compatible, but UFS2Tool has not been tested on .NET 10 here. Dokan P/Invoke goes through netstandard2.0, which is fine.
- **Test:** on a clean Windows VM with **only .NET 10**, run newfs (build .ffpkg), `mount_udf` / `umount_udf` (with Dokany 2.x) and `extract`. Repeat with only .NET 8 to check nothing regressed.

### R5. UFS2Tool 4.0.0 (c123639) → v4.1 with DokanNet 2.3.0.3 — CONSIDER (post-Farewell)
- **Why:** upstream fixes (`fsck_ufs -b`, `newfs -S 4096`) and a newer DokanNet, which probably has better Dokany 2.3 compatibility. It also gives a traceable build from a release tag instead of a pre-tag commit.
- **Risk: medium.** It is a new native and managed binary pair. Its newfs output must be validated on a PS5. The `-S 4096` fix does not affect current use (`-S 512`). Upstream still targets net8.0, so R4 is still needed.
- **Test:** build from tag `v4.1` yourself (`dotnet publish -c Release -r win-x64 --self-contained false`), record the SHA-256 values, replace all three blobs together, then run the R4 tests plus loading a built `.ffpkg` on a PS5.

### R6. tkinterdnd2 0.6.2 → 0.7.0, and drop `--collect-all tkinterdnd2` — UPDATE (optional)
- **Why:** tkdnd 2.10.1 → 2.10.3. 0.7.0 ships its own PyInstaller hook (`hook-dirs` entry point) that bundles only `win-x64`, which shrinks the exe and removes foreign binaries that look odd to antivirus heuristics. `TkinterDnD.py` is unchanged between the two versions.
- **Risk: low.** If the hook is not picked up, drag and drop quietly falls back to off; the app already handles `_DND_AVAILABLE=False`.
- **Test:** check that the PyInstaller build log shows `hook-tkinterdnd2`. In the built exe, drag a folder onto the Build tab. Check that the `_MEI` contents hold only `tkdnd/win-x64`.

### R7. mkpfs 0.0.9 → 1.1.0 — DEFER until after the Farewell release (or do it with full on-console validation)
- **Why update:** 3+ months of upstream fixes: bounded in-flight memory, Windows console fixes, a 64 KiB default exFAT cluster, and the PS5-unsafe ISA-L guard.
- **What breaks:**
  - mkpfs 1.1.0 makes **`customtkinter` a hard dependency** and adds `mkpfs.gui`. `--collect-all mkpfs` would bundle the GUI and customtkinter (plus darkdetect). If you update, use `--exclude-module mkpfs.gui --exclude-module customtkinter`, or `--collect-submodules mkpfs` with a filter.
  - Default changes include the exFAT cluster size for exFAT-wrapped output.
- **What I verified stays compatible:** the entry point `mkpfs.__main__.main(argv)`. Every flag the app passes: `pack file|folder`, `unpack`, `tree`, `verify`, `--deep`, `--cpu-count`, `--inode-bits`, `--no-compress`, `--no-adjust-output-file-extension`, `--version`, `--overwrite`, `--temp-folder`, `--compression-level`, `--source-dir`, `--verify`. The progress-line format (`[bar] NN% phase …`) is identical, so `_PROGRESS_RE` still matches. `mkpfs.exfat.open_exfat` exists.
- **Risk: medium–high.** This is the primary image engine, and its output must be proven on a PS5.
- **Test:** the test baseline. Then build the same three games (small, about 50 GB, and one with files over 4 GB) with 0.0.9 and with 1.1.0, compare `verify` and `tree --deep`, and boot them on a PS5.

### R8. isal 1.8.0 → 1.9.x — KEEP for now
- **Why wait:** 1.9.0 and 1.9.1 were both published today (2026-11-03). Give it at least two weeks.
- **Separately:** given upstream MkPFS issue #132 (ISA-L streams that the PS5 inflater rejects), consider labelling the Lazy "isa-l" backend option as risky in the UI, or removing it.
- **Test (when updating):** byte-compare Lazy ffpfsc output built with zlib against output built with isa-l. Boot the isa-l output on a PS5.

### R9. zlib-ng 1.0.0, psutil 7.2.2 — KEEP (already latest)
- Correct the `requirements-build.txt` comment: zlib-ng is **required** by mkpfs 0.0.9, not optional.

### R10. Lock the build environment — CHANGE (supply chain and reproducibility)
- Generate a fully pinned lock file with hashes (`pip-compile --generate-hashes`, or `uv pip compile --generate-hashes`), and install it with `pip install --require-hashes`. Pin the `pip` version in `build.bat`. Pin the GitHub Actions in `windows-build.yml` by commit SHA.
- **Risk: low.**
- **Test:** two CI runs of the same commit should produce identical `pip freeze` output. Exe hashes may still differ because of PyInstaller timestamps.

### R11. Python 3.11 → 3.13 — DEFER (post-Farewell)
- **Why:** 3.11 receives source-only security fixes; hand builds are stuck at 3.11.9 (from knowledge, unverified this session). Every dependency has cp313 wheels.
- **Risk: medium.** Multiprocessing, Tk 8.6.13+/9 and `ctypes` behaviour should be retested. The Lazy code already mentions Python 3.12+ (`build.py:449`). tkinterdnd2 ships Tcl9 builds.
- **Until then:** make **CI the only producer of release builds** (it already uses a current 3.11.x).
- **Test:** the full test baseline and an end-to-end build, extract and FTP pass on a Windows VM.

### R12. Notices — CHANGE (compliance)
- Do everything in §7. DokanNet is the main missing item.

### R13. Antivirus mitigations — CHANGE (post-Farewell)
- See §5 for the list (signing first; then onedir, un-embedding the PE files, and a rebuilt bootloader).

### Remove?
- None of the pinned packages are unused, so nothing should be removed.
- `tkinterweb` and `capstone` should stay **unbundled**. Their optional code paths can stay as they are.

---

## Other security-relevant observations (not dependency versions)

- **Elevated execution from a user-writable temp directory.** The app runs elevated and executes UFS2Tool.exe, the .bat/.ps1 and Backport.py from `tempfile.mkdtemp()` under the user's `%TEMP%`. A non-elevated process running as the same user can modify those files between the write and the exec, which is a privilege-boundary weakness. Mitigation: re-hash each file just before running it, or extract to a directory with an admin-only ACL. Un-embedding the binaries (§5) makes the re-hash easy.
- **Code from a user-chosen folder.** The `experimental_mkpfs_folder` override (`ui/experimental_mkpfs.py`) imports and runs Python from a folder the user picks, inside the elevated process. This is by design, but it deserves a warning in the UI.
- **Updater integrity.** The auto-updater checks the downloaded exe against a `.sha256` from the **same** GitHub release. That catches corruption but not a compromised release. Authenticode verification needs R13 (signing).
- Archive extraction (embedded Archive.py) checks every zip/tar member for path traversal and does not use `extractall`, so stdlib `tarfile` filter CVEs do not apply.

## Could not verify

- **UFS2Tool provenance:** the embedded UFS2Tool.exe/.dll could not be matched against an official GitHub release asset (REST API and github.com blocked). I identified it only from the version resource and the git history. DokanNet *was* matched against NuGet.
- **PyInstaller GHSA-9fxf-4qw3-ghmr:** severity, affected versions and CVE not confirmed. The advisory page and OSV are blocked, and PyPI has no record. Only its reference in PyInstaller's own changelog was verified.
- **GitHub issue text** for #63, #78 and MkPFS #132. Taken from the repo's README and CHANGELOG and from the MkPFS source comments.
- **Python lifecycle:** the 3.11 Windows-installer policy and EOL date come from knowledge (python.org and endoflife.date blocked).
- **Runtime behaviour:** no Windows build, run, AV scan or PS5 test was done. Every risk rating is from static analysis.
- **Unknown upstreams:** the Backport.py / Archive.py repo and version, and OSFMount's latest version.
- **robocopy `/COMPRESS`:** behaviour on Windows 10 network sources not verified.

Scratch artefacts (decoder, PE parser, downloaded wheels, upstream clones) are in `/tmp/claude-0/-home-user-ps5-exfat-builder/1641cfe0-4df8-5262-a4e7-af1d894f2c5b/scratchpad/agents/deps/`.
