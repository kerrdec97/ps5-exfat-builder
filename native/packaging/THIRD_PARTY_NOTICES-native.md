# Third-party notices — exFAT Image Builder v5 (native)

The native application is licensed under the GNU General Public License,
version 3 or later (LICENSES/GPL-3.0.txt). It contains or links the
following third-party components. Full licence texts are in LICENSES/.

| Component | Use | Licence |
|---|---|---|
| Qt 6 (Core, Gui, Widgets, Svg, Network, Concurrent, Sql) | Application framework, dynamically linked | LGPL-3.0 (https://www.qt.io/licensing). Qt source: https://download.qt.io/official_releases/qt/ |
| zlib | Compression | zlib licence |
| Lazy_MkPFS / PSBrew MkPFS | The PFS/FFPFSC engine is a C++ port of this code | GPL-3.0 |
| UFS2Tool (SvenGDK) | The UFS2/FFPKG engine follows its image layout | BSD-2-Clause (LICENSES/UFS2Tool-BSD-2-Clause.txt) |
| make_fself (john-tornblom, modified by Nazky) | The fake-SELF reader used by the Backport tab's game list (`engines/backport/SelfFormat.h`, `SelfReader.*`) is a port | GPL-3.0 |
| LibProsperoPKG (SvenGDK) | Facts used by the FPKG engine: PS5 flat-path hash, UCP layout | GPL-3.0-or-later |
| zstd 1.5.7 (Meta Platforms) | zstd asset packs: compression and decompression | BSD-3-Clause (LICENSES/zstd-BSD-3-Clause.txt) |
| LZ4 1.10.0 (Yann Collet), lib/lz4.c only | Reading (unpacking) LZ4 asset packs; the app never writes them | BSD-2-Clause (LICENSES/LZ4-BSD-2-Clause.txt) |
| ps5-dump-forge (quer3q) | The zstd pack writer and reader, the packing rules, the LZ4 (AMPRPAK4) reader and the image-wrapped .ffpfsc rule follow its formats and code (v0.5.4) | GPL-3.0-or-later |
| ampr-emu-ng (quer3q) | Format of the zstd packs (docs/zstd-format.md); its runtime is bundled (below) | GPL-3.0-or-later |
| Lucide icons | Interface icons | ISC (LICENSES/Lucide-ISC.txt) |
| Microsoft Visual C++ runtime (`vcruntime140*.dll`, `msvcp140*.dll`, ...) | Shipped next to the program inside the single-file exe | Microsoft Visual Studio redistributable files ("Distributable Code") |

## Bundled: the ampr-emu-ng runtime

`engines/ampr-emu-ng/libSceAmpr.sprx` is the unmodified release build of
**ampr-emu-ng 0.3.3** by quer3q (https://github.com/quer3q/ampr-emu-ng,
SHA-256 `1c1051f91bc7cba19b15362b2b832114b53a4c3d6193f50c90fdf1ae5d78d268`),
GPL-3.0-or-later. Its complete corresponding source is the tagged release:
https://github.com/quer3q/ampr-emu-ng/tree/v0.3.3 (also downloadable as
https://github.com/quer3q/ampr-emu-ng/archive/refs/tags/v0.3.3.tar.gz). It is a
PS5 module that the app copies, unchanged, to `fakelib/libSceAmpr.sprx` of a
game built with zstd packs; it runs on the console, not on the PC.

## Bundled separate program: PS5 Backport engine

`engines/backport/PS5BackportGUI.exe` is the unmodified release 0.3.4 of
**PS5 Backport GUI by Deckerr97** (https://github.com/kerrdec97/PS5-Backport-GUI,
SHA-256 `cf8eaa884f087a78f57ba1ae32cb28231393e622a0626a42ef4f1bd00804cf42`).
It is a separate program by the same author, distributed under its own terms
(see that repository's LICENSE.txt, NOTICE.md and THIRD_PARTY_NOTICES.txt),
and is not covered by this application's GPL licence. The Backport tab
starts it as a child process (`--worker job.json`) and exchanges JSON over
its standard streams; no code is linked. The libraries it carries and their
provenance are listed in its own notices (for example drakmor's ampr_emu,
GPL-3.0, redistributed unchanged).

No Sony software is included in the application itself. FPKG packages are
built with the user's own Publishing Tools, located through Settings > Tools.

The DLC page builds PS5 DLC packages with **fpkg-cli** from PSVIETHOA FPKG
Builder (Nguyễn Thanh Sơn and Ngô Phi Phương,
https://github.com/thanhsondev/PSVIETHOA-FPKG-Builder), the command-line front
end of drakmor's LibProsperoPkg engine (https://github.com/drakmor/LibProsperoPkg);
the DLC package builder is by Lapy. It is not included in the published
application either: the user points Settings > Tools > DLC engine at their
own fpkg-cli.exe. The app runs it as a child process; no code is linked.

No YouTube app package is included or downloaded. The Y2JB tab installs
packages from a folder the user chooses (their own copies).

Private builds (`make-release.ps1 -PrivateToolkit` / `-PrivateDlcEngine` /
`-PrivateYoutube`, output in `dist-private\`) carry the builder's own copies
of these tools and packages for personal use. They are never published.
