# THIRD-PARTY NOTICES

exFAT Image Builder incorporates, bundles, and/or distributes software from the
projects below. Each remains the property of its authors and is used under its
own license. Where a license requires it, the full license text must be included
and copyright notices preserved.

> ⚠️ **Maintainer action required.** Entries marked **[VERIFY]** were not
> confirmed against the upstream repository and must be checked before release.
> For every component, copy the exact copyright line from that project's own
> `LICENSE` file — do not paraphrase it.

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
- **Action:** place the GPL-3.0 license text in `vendor/lazy_mkpfs/LICENSE`,
  keep `UPSTREAM_README.md` and `UPSTREAM_COMMIT.txt`, and retain the
  "Based on PSBrew/MkPFS" attribution already present in the source.

### UFS2Tool — FreeBSD UFS1/UFS2 filesystem manager (`.ffpkg` backend)
- **Bundled as:** embedded `UFS2Tool.exe` (base64 in `exfat_builder.py`)
- **Author:** SvenGDK — <https://github.com/SvenGDK/UFS2Tool>
- **License: BSD-2-Clause** (permissive). Requires retaining the copyright
  notice and the two-clause license text in this file.
- **Action:** paste UFS2Tool's exact copyright line and the BSD-2-Clause text
  into the "License texts" section below.

### Fake-signing & decrypt tooling (backport engine)
Embedded as base64 in `exfat_builder.py` (`_SRC_FSELF_B64`, `_SRC_DECRYPT_B64`),
these convert ELF ⇄ fake-signed SELF and are wired into the Auto-Backport flow.
- **make_fself / decrypt_fself**
  - **Original author:** john-tornblom — from the PS5 payload SDK,
    <https://github.com/ps5-payload-dev/sdk> (`samples/install_app/make_fself.py`)
  - **Modified by:** Nazky (@NazkyYT)
  - **License: [VERIFY]** — confirm the ps5-payload-dev/sdk license and reproduce
    it (the source header credits john-tornblom; keep that header intact).

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
- **License: GPL-3.0.**
- **Upstream credits:** Drakmor (ShadowMountPlus); VoidWhisper (ShadowMount);
  BestPig (BackPort); EchoStretch (kstuff-toggle); Gezine; earthonion;
  LightningMods; john-tornblom (SDK); PS5 R&D community.

### ftpsrv — FTP server for jailbroken PS4/PS5
- **Author:** ps5-payload-dev — <https://github.com/ps5-payload-dev/ftpsrv>
- **License: [VERIFY]** (ps5-payload-dev projects are commonly permissive, but
  confirm on the repo before shipping the `.elf`).

### klogsrv — PS5 kernel log streamer
- **Author:** ps5-payload-dev — <https://github.com/ps5-payload-dev/klogsrv>
- **License: [VERIFY]**

### Other bundled payloads (if shipped in your release)
- ps5-hen, kstuff / kstuff-lite, zftpd, etc. — **[VERIFY]** author + license for
  each `.elf` you distribute, and list them here.

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

> Paste the full, verbatim license text for each component below, copied from
> that project's own `LICENSE` file. At minimum:
> - **GPL-3.0** (Lazy_MkPFS / MkPFS, and ShadowMountPlus if you ship it): the
>   full GNU GPL v3 text, once.
> - **BSD-2-Clause** (UFS2Tool): the two-clause text with SvenGDK's exact
>   copyright line.
> - Any text required by the **[VERIFY]** components once their licenses are
>   confirmed (ps5-payload-dev SDK / ftpsrv / klogsrv, idlesauce gist).

<!-- GPL-3.0 full text here -->

<!-- BSD-2-Clause text (UFS2Tool) here, with the exact copyright line from
     https://github.com/SvenGDK/UFS2Tool/blob/main/LICENSE -->
