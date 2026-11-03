# THIRD-PARTY NOTICES

exFAT Image Builder incorporates, bundles, and/or distributes software from the
projects below. Each remains the property of its authors and is used under its
own license. Where a license requires it, the full license text must be included
and copyright notices preserved.

> **Farewell Edition (v4.1.0) license review.** Every component below was
> checked against its upstream repository's `LICENSE` file where one exists;
> verified entries say so. Entries still marked **[VERIFY]** have no license
> file upstream (or could not be reached) and should be confirmed with their
> authors before redistribution. Full license texts are in `LICENSES/`.

---

## Incorporated / bundled into the application

### Lazy_MkPFS — PFS image build / compress / inspect backend
- **Vendored at:** `vendor/lazy_mkpfs/`
- **Author:** Nazky — <https://github.com/Nazky/Lazy_MkPFS>
- **Derived from:** MkPFS by PSBrew (Renan) — <https://github.com/PSBrew/MkPFS>
- **License: GPL-3.0.** Lazy_MkPFS is a derivative of PSBrew/MkPFS (GPL-3.0),
  so it carries GPL-3.0 terms. Because this source is vendored and distributed
  as part of exFAT Image Builder, the GPL-3.0 obligations extend to the
  distributed program (see LICENSING NOTE below).
- **Verified:** GPL-3.0, inherited from PSBrew/MkPFS. The repository's `main`
  branch carries the GPL-3.0 `LICENSE`; on the vendored `lazy` branch that file
  was removed upstream (commit `60a1722`), so the text was taken from `main`
  (byte-identical to PSBrew/MkPFS). The full text ships in `vendor/lazy_mkpfs/LICENSE` and `LICENSES/GPL-3.0.txt`;
  `UPSTREAM_README.md` / `UPSTREAM_COMMIT.txt` (upstream commit
  `78bdd00c5645b1170420df051c586dd0ca4f918d`) and the "Based on PSBrew/MkPFS"
  attribution are retained. Local vendoring patches (runtime `pip install`
  disabled) are marked in the source.

### Lucide — interface icons
- **Bundled as:** rendered icon masks in `ui/icons_data.py` (one custom
  "eject" glyph in the same style is original to this project)
- **Author:** Lucide Contributors (portions © Cole Bemis, Feather) —
  <https://lucide.dev>, <https://github.com/lucide-icons/lucide>, version 0.460.0
- **License: ISC — verified.** The full text ships in `LICENSES/Lucide-ISC.txt`.

### UFS2Tool — FreeBSD UFS1/UFS2 filesystem manager (`.ffpkg` backend)
- **Bundled as:** embedded `UFS2Tool.exe` (base64 in `exfat_builder.py`)
- **Author:** SvenGDK — <https://github.com/SvenGDK/UFS2Tool>
- **License: BSD-2-Clause — verified.** `Copyright (c) 2026, SvenGDK`. The
  full text is reproduced in "License texts" below and in
  `LICENSES/UFS2Tool-BSD-2-Clause.txt`.

### DokanNet — .NET wrapper for the Dokan user-mode file system
- **Bundled as:** embedded `DokanNet.dll` 2.0.1 (base64 in `exfat_builder.py`),
  used by UFS2Tool. Byte-identical to `lib/netstandard2.0/DokanNet.dll` in the
  official NuGet package `DokanNet 2.0.1`.
- **Authors:** Adrien J., Maxime C., Hiroki Asakawa —
  <https://github.com/dokan-dev/dokan-dotnet>
- **License: MIT — verified** (NuGet licence expression and the project's
  `license.txt`). The full text ships in `LICENSES/DokanNet-MIT.txt`.

### Bundled Python runtime and packages (Windows executable)
The release `.exe` is built with PyInstaller and contains these, each under
its own licence (versions in `requirements-build.txt`):
- **CPython** 3.11 — PSF License; **Tcl/Tk** — Tcl/Tk (BSD-style) licence.
- **PyInstaller bootloader** — GPL-2.0-or-later with the PyInstaller
  bootloader exception (the exception permits distributing the bundled app).
- **mkpfs** (PSBrew) — GPL-3.0 (`LICENSES/GPL-3.0.txt`).
- **Pillow** — MIT-CMU (HPND), with bundled libjpeg-turbo, libpng, zlib,
  libtiff, FreeType, OpenJPEG, libwebp and others under their own licences.
- **cryptography** — Apache-2.0 or BSD-3-Clause, with OpenSSL (Apache-2.0).
- **psutil** — BSD-3-Clause.
- **tkinterdnd2** — MIT, with the tkdnd library (BSD-style).
- **zlib-ng** (python-zlib-ng) — PSF-2.0, bundling zlib-ng (zlib licence).
- **isal** (python-isal) — PSF-2.0, bundling Intel ISA-L (BSD-3-Clause).
A complete listing with licence texts can be generated from the build
environment with `pip-licenses --from=mixed --with-license-file`.

### FPKG builder (not bundled: user-supplied tools)
`core/fpkg/` is original code written for this project. It drives the
user's own Sony Publishing Tools (`prospero-pub-cmd.exe`), which are **not**
included, linked or downloaded. No code from other FPKG builders was copied.

### Fake-signing & decrypt tooling (backport engine)
Embedded as base64 in `exfat_builder.py` (`_SRC_FSELF_B64`, `_SRC_DECRYPT_B64`),
these convert ELF ⇄ fake-signed SELF and are wired into the Auto-Backport flow.
- **make_fself / decrypt_fself**
  - **Original author:** john-tornblom — from the PS5 payload SDK,
    <https://github.com/ps5-payload-dev/sdk> (`samples/install_app/make_fself.py`)
  - **Modified by:** Nazky (@NazkyYT)
  - **License: GPL-3.0 — verified** from the ps5-payload-dev/sdk `LICENSE`
    file. The crediting header in the embedded source is kept intact.

### SDK version patcher (backport engine)
Embedded as base64 (`_SRC_PATCHER_B64`); rewrites PS5/PS4 SDK version fields.
- **Original author:** idlesauce — from the gist
  <https://gist.github.com/idlesauce/2ded24b7b5ff296f21792a8202542aaa>
- **Modified by:** Nazky (@NazkyYT)
- **License: [VERIFY]** — gists often carry no explicit license; confirm terms
  with the author before distribution. Keep the crediting header intact.

### Backport pipeline & Archive handler
Embedded as base64 (`_BACKPORT_B64`, `_SRC_ARCHIVE_B64`) — the orchestration that
drives the three tools above plus archive extraction. Part of Nazky's
Auto-Backport work; carries the licenses of the components it invokes.

### make_image.bat / PowerShell image builder
Embedded as base64 (`_BAT_B64`). Project's own build glue; invokes **OSFMount**
(OSForensics) at runtime — OSFMount is **not** bundled and is installed by the
user. No third-party code is shipped here.

---

## Distributed / sendable PS5 payloads

Sent to the console by the app. Separate programs — but if you redistribute the
`.elf` files, comply with each one's license.

### ShadowMountPlus / MicroMount — PS5 auto-mounter payload
- **Author:** drakmor — <https://github.com/drakmor/ShadowMountPlus>
  (fork of RDX-Sci01/ShadowMount1.3GBT; original ShadowMount by VoidWhisper)
- **License: GPL-3.0 — verified** from the upstream `LICENSE` file.
- **Upstream credits:** Drakmor (ShadowMountPlus); VoidWhisper (ShadowMount);
  BestPig (BackPort); EchoStretch (kstuff-toggle); Gezine; earthonion;
  LightningMods; john-tornblom (SDK); PS5 R&D community.

### ftpsrv — FTP server for jailbroken PS4/PS5
- **Author:** ps5-payload-dev — <https://github.com/ps5-payload-dev/ftpsrv>
- **License: GPL-3.0 — verified** from the upstream `LICENSE` file.

### klogsrv — PS5 kernel log streamer
- **Author:** ps5-payload-dev — <https://github.com/ps5-payload-dev/klogsrv>
- **License: GPL-3.0 — verified** from the upstream `LICENSE` file.

### Other bundled payloads (if shipped in your release)
- ps5-hen, kstuff / kstuff-lite, zftpd, etc. — **[VERIFY]** author + license for
  each `.elf` you distribute, and list them here. (No `LICENSE` file was found
  at the default branch of ps5-payload-dev/zftpd or EchoStretch/kstuff during
  the Farewell review.)

---

## Referenced research / inspiration (no code bundled)

- **PSBrew (Renan)** — original MkPFS project and PFS-format research.
- **BestPig** — PS5 backport research and the BackPork/BackPort payload
  (users download BackPork themselves; it is not bundled).
- **Nazky** — Auto-Backport pipeline and Lazy_MkPFS.
- **drakmor** — ShadowMount+/MicroMount, and reference work used for exFAT
  creation, DLC-emu, and the AMPR index generator.
- **john-tornblom** — PS5 payload SDK, the basis of the fake-signing tooling.
- **idlesauce** — original SDK version patcher.
- **NookieAI · stonemodder (Porkfolio)** — inspiration.
- **PS5 reverse-engineering community & PSDevWiki** — PFS/PKG/FPKG references.

---

## LICENSING NOTE (read before public release)

This project vendors and distributes **GPL-3.0-derived code** (Lazy_MkPFS, from
MkPFS). Under GPL-3.0's copyleft, a distributed program that incorporates GPL-3.0
code is normally required to be licensed under GPL-3.0 as a whole, with complete
corresponding source available and all notices preserved.

Practical options:
1. **License exFAT Image Builder under GPL-3.0** (add GPL-3.0 as the top-level
   `LICENSE`). Simplest path given the vendored code. Recommended.
2. **Remove the vendored Lazy_MkPFS** and instead call an externally-installed
   `mkpfs` the user provides. Larger change; would free the license choice.
   (Better suited to a future release, not a rushed change.)

This is a summary of common GPL mechanics, not legal advice. If in doubt, confirm
with the upstream authors and/or someone qualified before publishing.

---

## License texts

> Verbatim texts copied from each project's own `LICENSE` file. Still to add
> once confirmed: the terms for the idlesauce SDK-patcher gist and any other
> **[VERIFY]** payload you distribute.

### GNU General Public License v3.0
Applies to Lazy_MkPFS, MkPFS, ShadowMountPlus, the ps5-payload-dev SDK
tooling, ftpsrv and klogsrv. The complete, unmodified text is in
[`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt) (also shipped inside the
executable and at `vendor/lazy_mkpfs/LICENSE`).

### UFS2Tool — BSD 2-Clause License

```
BSD 2-Clause License

Copyright (c) 2026, SvenGDK
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
