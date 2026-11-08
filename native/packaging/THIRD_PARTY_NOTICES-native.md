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
| Lucide icons | Interface icons | ISC (LICENSES/Lucide-ISC.txt) |
| Microsoft Visual C++ runtime (`vcruntime140*.dll`, `msvcp140*.dll`, ...) | Shipped next to the program inside the single-file exe | Microsoft Visual Studio redistributable files ("Distributable Code") |

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
