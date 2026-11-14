# Third-party notices — exFAT Image Builder v5 (native)

exFAT Image Builder 5 is proprietary software (Copyright (c) 2026 Deckerr97,
all rights reserved; LICENCE.txt). It contains or links the following
third-party components, each of which keeps its own licence. Full licence
texts are in LICENSES/.

| Component | Use | Licence |
|---|---|---|
| Qt 6 (Core, Gui, Widgets, Svg, Network, Concurrent, Sql) | Application framework, dynamically linked | LGPL-3.0 (https://www.qt.io/licensing). Qt source: https://download.qt.io/official_releases/qt/. The Qt files carry third-party code of their own (below) |
| zlib | Compression | zlib licence |
| Lazy_MkPFS / PSBrew MkPFS | The PFS/FFPFSC engine is a C++ port of this code | GPL-3.0 |
| UFS2Tool (SvenGDK) | The UFS2/FFPKG engine follows its image layout | BSD-2-Clause (LICENSES/UFS2Tool-BSD-2-Clause.txt) |
| make_fself (john-tornblom, modified by Nazky) | The fake-SELF reader used by the Backport tab's game list (`engines/backport/SelfFormat.h`, `SelfReader.*`) is a port | GPL-3.0 |
| LibProsperoPKG (SvenGDK) | Facts used by the FPKG engine: PS5 flat-path hash, UCP layout | GPL-3.0-or-later |
| zstd 1.5.7 (Meta Platforms) | zstd asset packs: compression and decompression | BSD-3-Clause (LICENSES/zstd-BSD-3-Clause.txt) |
| LZ4 1.10.0 (Yann Collet), lib/lz4.c only | Reading (unpacking) LZ4 asset packs; the app never writes them | BSD-2-Clause (LICENSES/LZ4-BSD-2-Clause.txt) |
| libarchive 3.8.11 (Tim Kientzle and contributors), the 7z, RAR and RAR5 readers only | Reading 7z and RAR archives chosen as a build source | BSD-2-Clause (LICENSES/libarchive-BSD-2-Clause.txt) |
| liblzma from XZ Utils 5.8.4 (Lasse Collin and contributors), the library only | LZMA/LZMA2 decompression for 7z archives | 0BSD (LICENSES/XZ-liblzma-0BSD.txt) |
| ps5-dump-forge (quer3q) | The zstd pack writer and reader, the packing rules, the LZ4 (AMPRPAK4) reader and the image-wrapped .ffpfsc rule follow its formats and code (v0.5.4) | GPL-3.0-or-later |
| ampr-emu-ng (quer3q) | Format of the zstd packs (docs/zstd-format.md); its runtime is bundled (below) | GPL-3.0-or-later |
| Lucide icons | Interface icons | ISC (LICENSES/Lucide-ISC.txt) |
| Microsoft Visual C++ runtime (`vcruntime140*.dll`, `msvcp140*.dll`, ...) | Shipped next to the program (in the folder edition and inside the single-file exe) | Microsoft Visual Studio redistributable files ("Distributable Code") |

## Third-party code inside the Qt files

The Qt 6.10.3 libraries and plug-ins that ship with the program contain code
of other projects. The main parts are listed here; every part, with its
copyright notices and full licence text as the Qt sources give them, is in
`LICENSES/Qt-third-party-code.txt`.

| Component | In | Licence | Copyright |
|---|---|---|---|
| PCRE2 10.47 (with its JIT compiler, sljit) | Qt6Core | BSD-3-Clause with the PCRE2 exception; sljit BSD-2-Clause | University of Cambridge; Philip Hazel; Zoltan Herczeg |
| double-conversion 3.4.0 | Qt6Core | BSD-3-Clause | the V8 project authors |
| zlib 1.3.2 | Qt6Core, Qt6Gui, Qt6Network | zlib | Jean-loup Gailly and Mark Adler |
| TinyCBOR 7.0 | Qt6Core | MIT | Intel Corporation |
| Unicode Character Database 36, CLDR 48.1 | Qt6Core | Unicode-3.0 | Unicode, Inc. |
| Easing equations, SHA-384/512 (RFC 6234), SHA-3 brg_endian, Apache Tika MIME types | Qt6Core | BSD-3-Clause, BSD-3-Clause, BSD-2-Clause, Apache-2.0 | Robert Penner; IETF Trust; Brian Gladman; The Apache Software Foundation |
| FreeType 2.14.3 (and its anti-aliasing rasteriser) | Qt6Gui | FreeType License (FTL) | David Turner, Robert Wilhelm, Werner Lemberg and others |
| HarfBuzz 13.2.1 | Qt6Gui | MIT | Google, Behdad Esfahbod, Red Hat and others |
| libpng 1.6.56 | Qt6Gui | libpng | The PNG Reference Library Authors and others |
| MD4C 0.5.2 | Qt6Gui | MIT | Martin Mitáš |
| Emoji Segmenter 0.4.0, Vulkan Memory Allocator 3.2.1, D3D12 Memory Allocator, Khronos headers, Adobe Glyph List and others | Qt6Gui | Apache-2.0, MIT, BSD-3-Clause and others (see the file) | Google; Advanced Micro Devices; The Khronos Group; Adobe Systems and others |
| libpsl, Public Suffix List | Qt6Network | BSD-3-Clause; MPL-2.0 | The Chromium Authors; publicsuffix.org contributors |
| XSVG | Qt6Svg | HPND (sell variant) | USC/Information Sciences Institute |
| libjpeg-turbo 3.1.4 | plugins/imageformats/qjpeg.dll | IJG and BSD-3-Clause | D. R. Commander, Thomas G. Lane (Independent JPEG Group) and others |
| libwebp 1.6.0 | plugins/imageformats/qwebp.dll | BSD-3-Clause | Google Inc. |
| Wintab API | plugins/platforms/qwindows.dll | LCS/Telegraphics licence | LCS/Telegraphics |
| SQLite 3.51.3 | plugins/sqldrivers/qsqlite.dll | public domain | — |

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
and is not covered by the licence of this application. The Backport tab
starts it as a child process (`--worker job.json`) and exchanges JSON over
its standard streams; no code is linked. The libraries it carries and their
provenance are listed in its own notices (for example drakmor's ampr_emu,
GPL-3.0, redistributed unchanged).

<!-- public-edition: make-release.ps1 leaves this part out of a private build -->
No Sony software is included in the application itself. FPKG packages are
built with the user's own Publishing Tools, located through Settings > Tools.
<!-- /public-edition -->

The DLC page builds PS5 DLC packages with **fpkg-cli** from PSVIETHOA FPKG
Builder (Nguyễn Thanh Sơn and Ngô Phi Phương,
https://github.com/thanhsondev/PSVIETHOA-FPKG-Builder), the command-line front
end of drakmor's LibProsperoPkg engine (https://github.com/drakmor/LibProsperoPkg);
the DLC package builder is by Lapy.
<!-- public-edition -->
It is not included in the published application either: the user points
Settings > Tools > DLC engine at their own fpkg-cli.exe.
<!-- /public-edition -->
The app runs it as a child process; no code is linked.

<!-- public-edition -->
No YouTube app package is included or downloaded. The Y2JB tab installs
packages from a folder the user chooses (their own copies).
<!-- /public-edition -->

Private builds (`make-release.ps1 -PrivateToolkit` / `-PrivateDlcEngine` /
`-PrivateYoutube`, output in `dist-private\`) also carry the builder's own
copies of some of these tools and packages. Such an edition says so in its own
copy of this file, which lists what it carries.
